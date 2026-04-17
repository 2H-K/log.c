# Enhanced Log Library Makefile

CC = gcc
CFLAGS = -std=c17 -Wall -Wextra -pedantic -g -O2 -pthread
LDFLAGS = -pthread

# Source files
LIB_SRC = src/log.c
TEST_SRC = src/test_log.c
EXAMPLE_SRC = src/example.c

# Object files
LIB_OBJ = $(LIB_SRC:.c=.o)
TEST_OBJ = $(TEST_SRC:.c=.o)
EXAMPLE_OBJ = $(EXAMPLE_SRC:.c=.o)

# Targets
LIB_NAME = liblog.a
TEST_BIN = test_log
EXAMPLE_BIN = example

.PHONY: all clean test example

all: $(LIB_NAME) test example

# Compile library
$(LIB_OBJ): %.o: %.c src/log.h
	$(CC) $(CFLAGS) -c $< -o $@

# Create static library
$(LIB_NAME): $(LIB_OBJ)
	ar rcs $@ $^

# Compile test
$(TEST_BIN): $(TEST_OBJ) $(LIB_OBJ)
	$(CC) $(CFLAGS) $(TEST_OBJ) $(LIB_OBJ) -o $(TEST_BIN) $(LDFLAGS)

# Compile example
$(EXAMPLE_BIN): $(EXAMPLE_OBJ) $(LIB_OBJ)
	$(CC) $(CFLAGS) $(EXAMPLE_OBJ) $(LIB_OBJ) -o $(EXAMPLE_BIN) $(LDFLAGS)

# Run tests
test: $(TEST_BIN)
	./$(TEST_BIN)

# Run example
example: $(EXAMPLE_BIN)
	./$(EXAMPLE_BIN)

clean:
	rm -f $(LIB_OBJ) $(TEST_OBJ) $(EXAMPLE_OBJ)
	rm -f $(LIB_NAME) $(TEST_BIN) $(EXAMPLE_BIN)
	rm -f test_output.log* test_rotation* *.log

# Debug build
debug: CFLAGS += -DDEBUG -g3 -O0
debug: clean all

# Release build
release: CFLAGS += -O3 -DNDEBUG
release: clean all
