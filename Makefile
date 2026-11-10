CC ?= gcc
CFLAGS = -Wall -Wextra -O2 -g -std=c99 -Iinclude
SRC = $(wildcard src/*.c)
OBJ = $(SRC:.c=.o)
TARGET = mini_sql

TEST_SRC = tests/test_suite.c src/types.c src/disk_manager.c src/slotted_page.c src/buffer_pool.c src/bplus_tree.c src/catalog.c src/parser.c src/planner.c src/executor.c
TEST_TARGET = test_suite

ifeq ($(OS),Windows_NT)
    TARGET_BIN = $(TARGET).exe
    TEST_TARGET_BIN = $(TEST_TARGET).exe
    RM = del /Q /F
else
    TARGET_BIN = $(TARGET)
    TEST_TARGET_BIN = $(TEST_TARGET)
    RM = rm -f
endif

all: $(TARGET_BIN)

$(TARGET_BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

tests/%.o: tests/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TEST_TARGET_BIN): $(TEST_SRC)
	$(CC) $(CFLAGS) -o $@ $^

test: $(TEST_TARGET_BIN)
	@echo Running automated database test suite...
	./$(TEST_TARGET_BIN)

run: $(TARGET_BIN)
	./$(TARGET_BIN)

benchmark: $(TARGET_BIN)
	@echo Running automated database benchmark...
	@printf ".benchmark 100000\n.exit\n" | ./$(TARGET_BIN)

valgrind: $(TARGET_BIN)
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./$(TARGET_BIN)

clean:
	@rm -f src/*.o tests/*.o $(TARGET_BIN) $(TARGET) $(TEST_TARGET_BIN) $(TEST_TARGET) minisql.dat test_*.dat 2>/dev/null || $(RM) src\*.o tests\*.o $(TARGET_BIN) $(TARGET) $(TEST_TARGET_BIN) $(TEST_TARGET) minisql.dat test_*.dat 2>nul || true

.PHONY: all clean run benchmark valgrind test
