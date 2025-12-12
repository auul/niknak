#ifndef NK_GC_H
#define NK_GC_H

#include <stdatomic.h>
#include <stddef.h>

/* Configuration Knobs */

#define NK_GC_ARENA_PAGES 8
#define NK_GC_BEST_FIT    0
#define NK_GC_TAGGED_SIZE PAGE_SIZE * 2
#define NK_GC_LARGE_SIZE  PAGE_SIZE
#define NK_GC_CONCURRENT  0

/* Type Declarations */

typedef struct nk_gc {
    struct nk_gc_tag *tag;
    struct nk_gc_arena *arena;

    atomic_uchar mode;
    atomic_uintptr_t spare_work;
    atomic_uintptr_t live_work;
    struct nk_gc_work *work;
} nk_gc;

int gc_init(nk_gc *gc);
int gc_exit(nk_gc *gc);
void *gc_alloc(nk_gc *gc, size_t size);
bool gc_mark(nk_gc *gc, void *ptr);
bool gc_collect(nk_gc *gc);

#endif
