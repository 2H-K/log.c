# Enhanced Log Library Makefile

CC = gcc
CFLAGS = -std=c17 -Wall -Wextra -pedantic -g -O2 -pthread
LDFLAGS = -pthread

# Source files
LIB_SRC = src/log.c
TEST_SRC = src/test_log.c
EXAMPLE_SRC = src/example.c
THREAD_SAFETY_EXAMPLE_SRC = src/example_thread_safety.c

# Object files
LIB_OBJ = $(LIB_SRC:.c=.o)
TEST_OBJ = $(TEST_SRC:.c=.o)
EXAMPLE_OBJ = $(EXAMPLE_SRC:.c=.o)
THREAD_SAFETY_EXAMPLE_OBJ = $(THREAD_SAFETY_EXAMPLE_SRC:.c=.o)

# Targets
LIB_NAME = liblog.a
TEST_BIN = test_log
EXAMPLE_BIN = example
THREAD_SAFETY_EXAMPLE_BIN = example_thread_safety

.PHONY: all clean test example thread_safety

all: $(LIB_NAME) test example thread_safety

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

# Compile thread safety example
$(THREAD_SAFETY_EXAMPLE_BIN): $(THREAD_SAFETY_EXAMPLE_OBJ) $(LIB_OBJ)
	$(CC) $(CFLAGS) $(THREAD_SAFETY_EXAMPLE_OBJ) $(LIB_OBJ) -o $(THREAD_SAFETY_EXAMPLE_BIN) $(LDFLAGS)

# Run tests
test: $(TEST_BIN)
	./$(TEST_BIN)

# Run example
run_example: $(EXAMPLE_BIN)
	./$(EXAMPLE_BIN)

# Run thread safety example
thread_safety: $(THREAD_SAFETY_EXAMPLE_BIN)
	./$(THREAD_SAFETY_EXAMPLE_BIN)

clean:
	rm -f $(LIB_OBJ) $(TEST_OBJ) $(EXAMPLE_OBJ) $(THREAD_SAFETY_EXAMPLE_OBJ)
	rm -f $(LIB_NAME) $(TEST_BIN) $(EXAMPLE_BIN) $(THREAD_SAFETY_EXAMPLE_BIN)
	rm -f test_output.log* test_rotation* *.log
	rm -f example_*.log

# Debug build
debug: CFLAGS += -DDEBUG -g3 -O0
debug: clean all

# Release build
release: CFLAGS += -O3 -DNDEBUG
release: clean all
