OUT := niknak
LIB_FLAGS :=

CC := gcc
C_FLAGS := -O3 -Wall -MMD -MP
SRC_DIR := src
INC_DIR := include
OBJ_DIR := build

SRC_FILES := $(wildcard $(SRC_DIR)/*.c)
OBJ_FILES := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRC_FILES))

all: $(OUT)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OUT): $(OBJ_DIR) $(OBJ_FILES)
	$(CC) -o $(OUT) $(LIB_FLAGS) $(OBJ_FILES)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) -I$(INC_DIR) $(C_FLAGS) -c "$<" -o "$@"

-include $(OBJ_FILES:.o=.d)

test: $(OUT)
	./$(OUT)

clean:
	rm -rf $(OBJ_DIR)
	rm -f $(OUT)

.PHONY:
	all clean test
