#include "gc.h"
#include <limits.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ALIGN_PAD(address, align) (((align) - ((address) % (align))) % (align))

/* Forward Declarations */

static void mark_span(nk_gc *gc, void *ptr, size_t size);
static void mark_ptr(nk_gc *gc, void *ptr);

/* Tags */

typedef struct nk_gc_tag {
    struct nk_gc_tag *prev;
    size_t size;
    bool mark;
    char data[];
} nk_gc_tag;

static const size_t TAG_HEADER = offsetof(nk_gc_tag, data);
static const size_t TAG_DATA_OFFSET =
    TAG_HEADER + ALIGN_PAD(TAG_HEADER, alignof(max_align_t));
static const size_t TAG_MAX = SIZE_MAX - TAG_DATA_OFFSET;

static inline nk_gc_tag *new_tag(nk_gc *gc, size_t size)
{
    if (size > TAG_MAX) {
        // TODO ERROR
        return NULL;
    }

    nk_gc_tag *tag = malloc(TAG_DATA_OFFSET + size);
    if (!tag) {
        // TODO ERROR
        return NULL;
    }

    memset(tag, 0, TAG_HEADER);
    tag->prev = gc->tag;
    tag->size = size;
    gc->tag = tag;

    return tag;
}

static inline void destroy_tag(nk_gc_tag *tag)
{
    free(tag);
}

static inline void *get_tag_data(const nk_gc_tag *tag)
{
    return (char *)tag + TAG_DATA_OFFSET;
}

static inline bool is_on_tag(const nk_gc_tag *tag, uintptr_t uptr)
{
    uintptr_t data_start = (uintptr_t)get_tag_data(tag);
    uintptr_t data_end = data_start + tag->size;
    return data_start <= uptr && uptr < data_end;
}

static inline void mark_tag(nk_gc *gc, nk_gc_tag *tag)
{
    if (tag->mark) {
        return;
    }
    tag->mark = true;
    mark_span(gc, get_tag_data(tag), tag->size);
}

/* Bitsets */

typedef uintmax_t nk_gc_bitset;

static const size_t BUCKET_SIZE = alignof(max_align_t);
static const size_t PAGE_BUCKETS = sizeof(nk_gc_bitset) * CHAR_BIT;
static const size_t PAGE_SIZE = PAGE_BUCKETS * BUCKET_SIZE;
static const nk_gc_bitset FULL_PAGE_MASK = ~(nk_gc_bitset)0;
static const unsigned FIT_NOT_FOUND = (unsigned)-1;

static inline unsigned get_bucket_page(unsigned bucket)
{
    return bucket / PAGE_BUCKETS;
}

static inline unsigned get_page_offset(unsigned bucket)
{
    return bucket % PAGE_BUCKETS;
}

static inline bool get_offset_bit(nk_gc_bitset page_bitset, unsigned offset)
{
    return 1 & (page_bitset >> offset);
}

static inline bool
get_page_bit(nk_gc_bitset *bitset, unsigned page, unsigned offset)
{
    return get_offset_bit(bitset[page], offset);
}

static inline bool get_bucket_bit(nk_gc_bitset *bitset, unsigned index)
{
    return get_page_bit(bitset, get_bucket_page(index), get_page_offset(index));
}

static inline void
set_page_bit(nk_gc_bitset *bitset, unsigned page, unsigned offset)
{
    bitset[page] |= (nk_gc_bitset)1 << offset;
}

static inline void set_bucket_bit(nk_gc_bitset *bitset, unsigned index)
{
    set_page_bit(bitset, get_bucket_page(index), get_page_offset(index));
}

static inline nk_gc_bitset build_page_mask(unsigned width)
{
    return FULL_PAGE_MASK >> (PAGE_BUCKETS - width);
}

static inline void
set_bitset_span(nk_gc_bitset *bitset, unsigned index, unsigned width)
{
    unsigned start_page = get_bucket_page(index);
    unsigned start_offset = get_page_offset(index);
    unsigned end_page = get_bucket_page(index + width);
    unsigned end_offset = get_page_offset(index + width);

    if (start_page == end_page) {
        bitset[start_page] |= build_page_mask(width) << start_offset;
        return;
    }

    bitset[start_page] |= build_page_mask(PAGE_BUCKETS - start_offset)
                          << start_offset;
    for (unsigned page = start_page + 1; page < end_page; page++) {
        bitset[page] = FULL_PAGE_MASK;
    }
    bitset[end_page] |= build_page_mask(end_offset);
}

static inline unsigned
bitset_first_fit(const nk_gc_bitset *bitset, unsigned width)
{
    unsigned run_span = 0;
    unsigned run_index = 0;
    unsigned at = 0;

    for (unsigned page = 0; page < NK_GC_ARENA_PAGES; page++) {
        nk_gc_bitset page_bitset = bitset[page];
        for (unsigned i = 0; i < PAGE_BUCKETS; i++, at++) {
            if (get_offset_bit(page_bitset, i)) {
                run_span = 0;
            } else {
                if (run_span == 0) {
                    run_index = at;
                }
                run_span++;
                if (run_span == width) {
                    return run_index;
                }
            }
        }
    }
    return FIT_NOT_FOUND;
}

static inline unsigned
bitset_best_fit(unsigned *span_dest, const nk_gc_bitset *bitset, unsigned width)
{
    unsigned best_span = FIT_NOT_FOUND;
    unsigned best_index = FIT_NOT_FOUND;
    unsigned run_span = 0;
    unsigned run_index = 0;
    unsigned at = 0;

    for (unsigned page = 0; page < NK_GC_ARENA_PAGES; page++) {
        nk_gc_bitset page_bitset = bitset[page];
        for (unsigned i = 0; i < PAGE_BUCKETS; i++, at++) {
            if (get_offset_bit(page_bitset, i)) {
                if (width == run_span) {
                    *span_dest = run_span;
                    return run_index;
                } else if (width < run_span && run_span < best_span) {
                    best_span = run_span;
                    best_index = run_index;
                }
                run_span = 0;
            } else {
                if (run_span == 0) {
                    run_index = at;
                }
                run_span++;
            }
        }
    }

    if (width <= run_span && run_span < best_span) {
        *span_dest = run_span;
        return run_index;
    }

    *span_dest = best_span;
    return best_index;
}

/* Arenas */

typedef max_align_t nk_gc_bucket;

typedef struct nk_gc_arena {
    struct nk_gc_arena *prev;
    nk_gc_bitset first[NK_GC_ARENA_PAGES];
    nk_gc_bitset last[NK_GC_ARENA_PAGES];
    nk_gc_bitset full[NK_GC_ARENA_PAGES];
    nk_gc_bitset mark[NK_GC_ARENA_PAGES];
    char data[];
} nk_gc_arena;

static const size_t ARENA_HEADER = offsetof(nk_gc_arena, data);
static const size_t ARENA_DATA_OFFSET =
    ARENA_HEADER + ALIGN_PAD(ARENA_HEADER, alignof(nk_gc_bucket));
static const size_t ARENA_BUCKETS = NK_GC_ARENA_PAGES * PAGE_BUCKETS;
static const size_t ARENA_SIZE =
    ARENA_DATA_OFFSET + ARENA_BUCKETS * BUCKET_SIZE;

static inline unsigned get_bucket_width(size_t size)
{
    return (size + BUCKET_SIZE - 1) / BUCKET_SIZE;
}

static inline nk_gc_arena *new_arena(nk_gc *gc)
{
    nk_gc_arena *arena = malloc(ARENA_SIZE);
    if (!arena) {
        // TODO ERROR
        return NULL;
    }

    memset(arena, 0, sizeof(nk_gc_arena));
    arena->prev = gc->arena;
    gc->arena = arena;

    return arena;
}

static inline void destroy_arena(nk_gc_arena *arena)
{
    free(arena);
}

static inline void *get_bucket_ptr(nk_gc_arena *arena, unsigned index)
{
    return (char *)arena + ARENA_DATA_OFFSET + index * BUCKET_SIZE;
}

static inline void *
reserve_bucket(nk_gc_arena *arena, unsigned index, unsigned width)
{
    set_bitset_span(arena->full, index, width);
    set_bucket_bit(arena->first, index);
    set_bucket_bit(arena->last, index + width - 1);

    return get_bucket_ptr(arena, index);
}

static inline void *reserve_first_fit(nk_gc *gc, unsigned width)
{
    for (nk_gc_arena *arena = gc->arena; arena; arena = arena->prev) {
        unsigned index = bitset_first_fit(arena->full, width);
        if (index != FIT_NOT_FOUND) {
            return reserve_bucket(arena, index, width);
        }
    }
    return NULL;
}

static inline void *reserve_best_fit(nk_gc *gc, unsigned width)
{
    nk_gc_arena *best_arena = NULL;
    unsigned best_span = FIT_NOT_FOUND;
    unsigned best_index = FIT_NOT_FOUND;

    for (nk_gc_arena *arena = gc->arena; arena; arena = arena->prev) {
        unsigned fit_span;
        unsigned index = bitset_best_fit(&fit_span, arena->full, width);
        if (fit_span == width) {
            return reserve_bucket(arena, index, width);
        } else if (fit_span < best_span) {
            best_arena = arena;
            best_index = index;
            best_span = fit_span;
        }
    }
    return best_arena ? reserve_bucket(best_arena, best_index, best_span)
                      : NULL;
}

#if NK_ALLOC_BEST_FIT
static inline void *arena_reserve(nk_gc *gc, unsigned width)
{
    return reserve_best_fit(gc, width);
}
#else
static inline void *arena_reserve(nk_gc *gc, unsigned width)
{
    return reserve_first_fit(gc, width);
}
#endif

static inline bool is_on_arena(const nk_gc_arena *arena, uintptr_t address)
{
    uintptr_t arena_start = (uintptr_t)arena + ARENA_DATA_OFFSET;
    uintptr_t arena_end = ARENA_SIZE - arena_start;
    return arena_start <= address && address < arena_end;
}

static inline unsigned
get_address_bucket(const nk_gc_arena *arena, uintptr_t address)
{
    uintptr_t arena_start = (uintptr_t)arena + ARENA_DATA_OFFSET;
    return (address - arena_start) / BUCKET_SIZE;
}

static inline void *
arena_mark_to_first(nk_gc_arena *arena, unsigned page, unsigned offset)
{
    while (!get_page_bit(arena->first, page, offset)) {
        set_page_bit(arena->mark, page, offset);
        if (offset) {
            offset--;
        } else {
            offset = PAGE_BUCKETS - 1;
            page--;
        }
    }
    set_page_bit(arena->mark, page, offset);
    return get_bucket_ptr(arena, page * PAGE_BUCKETS + offset);
}

static inline void *
arena_mark_to_last(nk_gc_arena *arena, unsigned page, unsigned offset)
{
    while (!get_page_bit(arena->last, page, offset)) {
        set_page_bit(arena->mark, page, offset);
        offset++;
        if (offset == PAGE_BUCKETS) {
            offset = 0;
            page++;
        }
    }
    set_page_bit(arena->mark, page, offset);
    return get_bucket_ptr(arena, page * PAGE_BUCKETS + offset);
}

static inline void
arena_mark_object(nk_gc *gc, nk_gc_arena *arena, uintptr_t address)
{
    unsigned index = get_address_bucket(arena, address);
    unsigned page = get_bucket_page(index);
    unsigned offset = get_page_offset(index);
    if (get_page_bit(arena->mark, page, offset) ||
        !get_page_bit(arena->full, page, offset)) {
        return;
    }

    uintptr_t first_uptr = (uintptr_t)arena_mark_to_first(arena, page, offset);
    uintptr_t last_uptr = (uintptr_t)arena_mark_to_last(arena, page, offset);
    mark_span(gc, (void *)first_uptr, last_uptr - first_uptr + BUCKET_SIZE);
}

static inline bool is_arena_unmarked(const nk_gc_arena *arena)
{
    for (unsigned i = 0; i < NK_GC_ARENA_PAGES; i++) {
        if (arena->mark[i]) {
            return false;
        }
    }
    return true;
}

static inline void rebuild_arena(nk_gc_arena *arena)
{
    for (unsigned i = 0; i < NK_GC_ARENA_PAGES; i++) {
        arena->first[i] &= arena->mark[i];
        arena->last[i] &= arena->mark[i];
    }
    memcpy(arena->full, arena->mark, sizeof(arena->mark));
    memset(arena->mark, 0, sizeof(arena->mark));
}

/* Allocation */

static inline void *alloc_tagged(nk_gc *gc, size_t size)
{
    nk_gc_tag *tag = new_tag(gc, size);
    if (!tag) {
        // TODO ERROR
        return NULL;
    }
    return get_tag_data(tag);
}

static inline void *alloc_large(nk_gc *gc, size_t size)
{
    void *ptr = arena_reserve(gc, get_bucket_width(size));
    if (ptr) {
        return ptr;
    }
    return alloc_tagged(gc, size);
}

static inline void *alloc_med(nk_gc *gc, size_t size)
{
    unsigned width = get_bucket_width(size);
    void *ptr = arena_reserve(gc, width);
    if (ptr) {
        return ptr;
    }

    nk_gc_arena *arena = new_arena(gc);
    if (!arena) {
        // TODO ERROR
        return NULL;
    }
    return reserve_bucket(arena, 0, width);
}

static inline void *alloc_bucket(nk_gc *gc)
{
    // TODO STUB
    return alloc_med(gc, BUCKET_SIZE);
}

static inline void *alloc_small(nk_gc *gc, size_t size)
{
    // TODO STUB
    return alloc_med(gc, size);
}

void *gc_alloc(nk_gc *gc, size_t size)
{
    if (size >= NK_GC_TAGGED_SIZE) {
        return alloc_tagged(gc, size);
    } else if (size >= NK_GC_LARGE_SIZE) {
        return alloc_tagged(gc, size);
    } else if (size > BUCKET_SIZE) {
        return alloc_med(gc, size);
    } else if (size == BUCKET_SIZE) {
        return alloc_bucket(gc);
    } else {
        return alloc_small(gc, size);
    }
}

/* Work Lists */

typedef struct nk_gc_work {
    struct nk_gc_work *prev;
    void *ptr;
} nk_gc_work;

static inline nk_gc_work *blank_work(nk_gc *gc, void *ptr)
{
    uintptr_t spare_uptr =
        atomic_load_explicit(&gc->spare_work, memory_order_acquire);
    if (spare_uptr) {
        return (void *)spare_uptr;
    }
    return gc_alloc(gc, sizeof(nk_gc_work));
}

static inline bool push_work_item(nk_gc *gc, void *ptr)
{
    nk_gc_work *work = blank_work(gc, ptr);
    if (!work) {
        // TODO ERROR
        return false;
    }
    work->prev = gc->work;
    work->ptr = ptr;
    gc->work = work;
    return true;
}

static inline void
push_work_node_threadsafe(atomic_uintptr_t *dest, nk_gc_work *work)
{
    uintptr_t prev_uptr = atomic_load_explicit(dest, memory_order_acquire);
    work->prev = (void *)prev_uptr;

    while (!atomic_compare_exchange_weak_explicit(
        dest,
        &prev_uptr,
        (uintptr_t)work,
        memory_order_release,
        memory_order_relaxed)) {
        work->prev = (void *)prev_uptr;
    }
}

static inline bool push_work_item_threadsafe(nk_gc *gc, void *ptr)
{
    nk_gc_work *work = blank_work(gc, ptr);
    if (!work) {
        // TODO ERROR
        return false;
    }
    work->ptr = ptr;
    push_work_node_threadsafe(&gc->live_work, work);
    return true;
}

static inline nk_gc_work *steal_live_work(nk_gc *gc)
{
    nk_gc_work *work = (void *)
        atomic_exchange_explicit(&gc->live_work, 0, memory_order_acq_rel);
    if (!work) {
        return NULL;
    }

    nk_gc_work *trace = work;
    while (trace->prev) {
        trace = trace->prev;
    }
    trace->prev = gc->work;
    gc->work = work;

    return work;
}

static inline nk_gc_work *pop_work(nk_gc *gc)
{
    nk_gc_work *work = gc->work;
    if (!work) {
        work = steal_live_work(gc);
        if (!work) {
            return NULL;
        }
    }
    gc->work = work->prev;

    return work;
}

static inline void handle_work_list(nk_gc *gc)
{
    for (nk_gc_work *work = pop_work(gc); work; work = pop_work(gc)) {
        void *ptr = work->ptr;
        push_work_node_threadsafe(&gc->spare_work, work);
        mark_ptr(gc, ptr);
    }
}

/* Garbage Collector */

enum {
    GC_MODE_OFF,
    GC_MODE_REQUEST,
    GC_MODE_WORKING,
    GC_MODE_FINISH,
};

int gc_init(nk_gc *gc)
{
    memset(gc, 0, sizeof(nk_gc));
    atomic_init(&gc->mode, GC_MODE_OFF);
    atomic_init(&gc->spare_work, 0);
    atomic_init(&gc->live_work, 0);
    return 0;
}

int gc_exit(nk_gc *gc)
{
    while (gc->tag) {
        nk_gc_tag *tag = gc->tag;
        gc->tag = tag->prev;
        destroy_tag(tag);
    }

    while (gc->arena) {
        nk_gc_arena *arena = gc->arena;
        gc->arena = arena->prev;
        destroy_arena(arena);
    }

    return 0;
}

static inline bool is_ptr_managed(const nk_gc *gc, const void *ptr)
{
    uintptr_t uptr = (uintptr_t)ptr;
    for (const nk_gc_tag *tag = gc->tag; tag; tag = tag->prev) {
        if (is_on_tag(tag, uptr)) {
            return true;
        }
    }

    for (nk_gc_arena *arena = gc->arena; arena; arena = arena->prev) {
        if (is_on_arena(arena, uptr)) {
            unsigned index = get_address_bucket(arena, uptr);
            return get_page_bit(
                arena->full,
                get_bucket_page(index),
                get_page_offset(index));
        }
    }

    return false;
}

bool gc_mark(nk_gc *gc, void *ptr)
{
    if (!is_ptr_managed(gc, ptr)) {
        return true;
    } else if (
        atomic_load_explicit(&gc->mode, memory_order_acquire) == GC_MODE_OFF) {
        return push_work_item(gc, ptr);
    } else {
        return push_work_item_threadsafe(gc, ptr);
    }
}

static void mark_ptr(nk_gc *gc, void *ptr)
{
    uintptr_t uptr = (uintptr_t)ptr;
    for (nk_gc_tag *tag = gc->tag; tag; tag = tag->prev) {
        if (is_on_tag(tag, uptr)) {
            mark_tag(gc, tag);
            return;
        }
    }

    for (nk_gc_arena *arena = gc->arena; arena; arena = arena->prev) {
        if (is_on_arena(arena, uptr)) {
            arena_mark_object(gc, arena, uptr);
            return;
        }
    }
}

static inline void mark_span(nk_gc *gc, void *ptr, size_t size)
{
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end = start + size;
    start += ALIGN_PAD(start, alignof(void *));
    end += ALIGN_PAD(end, alignof(void *));

    for (uintptr_t at = start; at < end; at += sizeof(void *)) {
        mark_ptr(gc, (void *)at);
    }
}

static inline void working_phase(nk_gc *gc)
{
    atomic_store_explicit(&gc->mode, GC_MODE_WORKING, memory_order_release);
    handle_work_list(gc);
    atomic_store_explicit(&gc->mode, GC_MODE_FINISH, memory_order_release);
}

static inline void finish_phase(nk_gc *gc)
{
    atomic_store_explicit(&gc->mode, GC_MODE_OFF, memory_order_relaxed);
    atomic_store_explicit(&gc->spare_work, 0, memory_order_relaxed);
    handle_work_list(gc);

    nk_gc_tag **tag_p = &gc->tag;
    while (*tag_p) {
        nk_gc_tag *tag = *tag_p;
        if (tag->mark) {
            tag->mark = false;
        } else {
            *tag_p = tag->prev;
            destroy_tag(tag);
        }
    }

    nk_gc_arena **arena_p = &gc->arena;
    while (*arena_p) {
        nk_gc_arena *arena = *arena_p;
        if (is_arena_unmarked(arena)) {
            *arena_p = arena->prev;
            destroy_arena(arena);
        } else {
            arena_p = &arena->prev;
            rebuild_arena(arena);
        }
    }
}

#if NK_GC_CONCURRENT
// TODO STUB
#else
bool gc_collect(nk_gc *gc)
{
    // TODO MARK ROOTS
    working_phase(gc);
    finish_phase(gc);
    return true;
}
#endif
