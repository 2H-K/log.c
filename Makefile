# ============================================================================
#  Makefile for log.c - Cross-platform static library + test builds
# ============================================================================

# ----- Detect OS -----
ifdef OS
  RM       = del /Q
  IS_WIN   = 1
else
  RM       = rm -f
  IS_WIN   =
endif

# ----- Compiler settings -----
ifneq ($(CC),cl)
  ifeq ($(origin CC), default)
    CC := gcc
  endif
  LIB_EXT  = .a
  AR       = ar
  TARGET   = liblogc.a
  ifdef IS_WIN
    LDFLAGS  =
  else
    LDFLAGS  = -lpthread
  endif
  CFLAGS  += -std=c11 -Wall -Wextra -Isrc -Itests
  TEST_EXT =
else
  LIB_EXT  = .lib
  AR       = lib
  TARGET   = logc.lib
  LDFLAGS  =
  CFLAGS  += /std:c11 /W4 /WX- /D_CRT_SECURE_NO_WARNINGS /Isrc /Itests
  TEST_EXT = .exe
endif

# ----- Directories -----
SRC_DIR   = src
TEST_DIR  = tests

# ----- Library -----
.PHONY: all lib clean run-tests

all: $(TARGET)

$(TARGET): $(SRC_DIR)/log.c $(SRC_DIR)/log.h
	$(CC) $(CFLAGS) -c $(SRC_DIR)/log.c -o log.o
	$(AR) rcs $@ log.o

# ----- Test runners -----
# Test files are included via #include in the runner .c file,
# so we only compile the runner + log.c (single compilation unit)

test_core$(TEST_EXT): $(TEST_DIR)/core.c $(wildcard $(TEST_DIR)/core/*.c) $(SRC_DIR)/log.c $(SRC_DIR)/log.h
	$(CC) $(CFLAGS) -o $@ $(TEST_DIR)/core.c $(SRC_DIR)/log.c $(LDFLAGS)

test_thread$(TEST_EXT): $(TEST_DIR)/thread.c $(wildcard $(TEST_DIR)/thread/*.c) $(SRC_DIR)/log.c $(SRC_DIR)/log.h
	$(CC) $(CFLAGS) -o $@ $(TEST_DIR)/thread.c $(SRC_DIR)/log.c $(LDFLAGS)

test_platform$(TEST_EXT): $(TEST_DIR)/platform.c $(wildcard $(TEST_DIR)/platform/*.c) $(SRC_DIR)/log.c $(SRC_DIR)/log.h
	$(CC) $(CFLAGS) -o $@ $(TEST_DIR)/platform.c $(SRC_DIR)/log.c $(LDFLAGS)

test_stress$(TEST_EXT): $(TEST_DIR)/stress.c $(wildcard $(TEST_DIR)/stress/*.c) $(SRC_DIR)/log.c $(SRC_DIR)/log.h
	$(CC) $(CFLAGS) -o $@ $(TEST_DIR)/stress.c $(SRC_DIR)/log.c $(LDFLAGS)

test_perf$(TEST_EXT): $(TEST_DIR)/perf.c $(wildcard $(TEST_DIR)/perf/*.c) $(SRC_DIR)/log.c $(SRC_DIR)/log.h
	$(CC) $(CFLAGS) -o $@ $(TEST_DIR)/perf.c $(SRC_DIR)/log.c $(LDFLAGS)

test_all$(TEST_EXT): $(TEST_DIR)/all.c $(wildcard $(TEST_DIR)/*/*.c) $(SRC_DIR)/log.c $(SRC_DIR)/log.h
	$(CC) $(CFLAGS) -o $@ $(TEST_DIR)/all.c $(SRC_DIR)/log.c $(LDFLAGS)

# ----- Run all tests -----
run-tests: test_core test_thread test_stress test_platform test_perf
	@echo ""
	@echo "=== Core Tests ==="
	-./test_core$(TEST_EXT)
	@echo ""
	@echo "=== Thread Tests ==="
	-./test_thread$(TEST_EXT)
	@echo ""
	@echo "=== Stress Tests ==="
	-./test_stress$(TEST_EXT)
	@echo ""
	@echo "=== Platform Tests ==="
	-./test_platform$(TEST_EXT)
	@echo ""
	@echo "=== Performance Benchmarks ==="
	-./test_perf$(TEST_EXT)

run-all: test_all
	@echo ""
	@echo "=== Running All Tests ==="
	-./test_all$(TEST_EXT)

# ----- Clean -----
clean:
	$(RM) *.o *.a *.lib $(TARGET) 2>nul || true
	$(RM) test_core$(TEST_EXT) test_thread$(TEST_EXT) test_stress$(TEST_EXT) 2>nul || true
	$(RM) test_platform$(TEST_EXT) test_perf$(TEST_EXT) test_all$(TEST_EXT) 2>nul || true
	$(RM) test_*.txt test_*.log test_*.json test_rot* test_h1.txt test_h2.txt 2>nul || true
