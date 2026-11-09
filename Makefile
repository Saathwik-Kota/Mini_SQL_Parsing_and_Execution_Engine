CC ?= gcc
CFLAGS = -Wall -Wextra -O2 -g -std=c99 -Iinclude
SRC = $(wildcard src/*.c)
OBJ = $(SRC:.c=.o)
TARGET = mini_sql

ifeq ($(OS),Windows_NT)
    TARGET_BIN = $(TARGET).exe
    RM = del /Q /F
else
    TARGET_BIN = $(TARGET)
    RM = rm -f
endif

all: $(TARGET_BIN)

$(TARGET_BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET_BIN)
	./$(TARGET_BIN)

benchmark: $(TARGET_BIN)
	@echo Running automated database benchmark...
	@printf ".benchmark 100000\n.exit\n" | ./$(TARGET_BIN)

valgrind: $(TARGET_BIN)
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./$(TARGET_BIN)

clean:
	@rm -f src/*.o $(TARGET_BIN) $(TARGET) minisql.dat 2>/dev/null || $(RM) src\*.o $(TARGET_BIN) $(TARGET) minisql.dat 2>nul || true

.PHONY: all clean run benchmark valgrind

