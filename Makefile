CC := gcc
CFLAGS := -std=c23 -Wall -Wextra -Werror -g -O1 -Iinclude -Itests
SRC := src/rbtree.c src/pool.c
TSRC := tests/test_rbtree.c tests/fault_alloc.c
BIN := build/test_rbtree
FUZZBIN := build/fuzz
all: $(BIN) $(FUZZBIN)
$(BIN): $(SRC) $(TSRC) include/rbtree.h tests/fault_alloc.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(TSRC) -o $@ -lpthread
$(FUZZBIN): $(SRC) tests/fuzz.c tests/fault_alloc.c include/rbtree.h tests/fault_alloc.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) tests/fuzz.c tests/fault_alloc.c -o $@ -lpthread
test: $(BIN) $(FUZZBIN)
	./$(BIN) && ./$(FUZZBIN) 100000
asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean test
memcheck: all
	valgrind --leak-check=full --show-leak-kinds=all \
		--error-exitcode=1 ./$(BIN)
	valgrind --leak-check=full --show-leak-kinds=all \
		--error-exitcode=1 ./$(FUZZBIN) 20000
clean:
	rm -rf build
.PHONY: all test asan memcheck clean
