# ============================================================================
#  Makefile for log.c — cross-platform static library + test builds
#  Compatible with: GCC (Linux), MinGW-w64 (Windows), MSVC (Windows)
# ============================================================================

# ----- Detect OS -----
ifdef OS
  RM       = del /Q
  IS_WIN   = 1
else
  RM       = rm -f
  IS_WIN   =
endif

# ----- Detect compiler & set platform flags -----
ifneq ($(CC),cl)
  # GCC / MinGW / Clang path
  ifeq ($(origin CC),default)
    CC := gcc
  endif
  LIB_PFX  = lib
  LIB_EXT  = .a
  AR       = ar
  TARGET   = liblogc.a
  ifdef IS_WIN
    LDFLAGS  =
  else
    LDFLAGS  = -lpthread
  endif
  CFLAGS  += -std=c11 -Wall -Wextra -Isrc
  TEST_EXT =
else
  # MSVC cl.exe path
  LIB_PFX  =
  LIB_EXT  = .lib
  AR       = lib
  TARGET   = logc.lib
  LDFLAGS  =
  CFLAGS  += /std:c11 /W4 /WX- /D_CRT_SECURE_NO_WARNINGS /Isrc
  TEST_EXT = .exe
endif

# ----- Directories -----
SRC_DIR  = src
TEST_DIR = tests

# ----- Test sources -----
TEST_SRCS = $(TEST_DIR)/benchmark.c $(TEST_DIR)/feature_test.c $(TEST_DIR)/thread_test.c $(TEST_DIR)/performance_test.c

# ----- Default: build library only -----
.PHONY: all lib tests clean run-tests

all: $(TARGET)

# ----- Static library -----
$(TARGET): $(SRC_DIR)/log.c $(SRC_DIR)/log.h
	$(CC) $(CFLAGS) -c $(SRC_DIR)/log.c -o log.o
	$(AR) rcs $@ log.o

# ----- Build all tests -----
tests: $(TEST_SRCS:$(TEST_DIR)/%.c=%$(TEST_EXT))

# Pattern rule for test executables
%$(TEST_EXT): $(TEST_DIR)/%.c $(SRC_DIR)/log.c $(SRC_DIR)/log.h
	$(CC) $(CFLAGS) -o $@ $< $(SRC_DIR)/log.c $(LDFLAGS)

# ----- Run all tests -----
run-tests: tests
	@echo "=== Running Benchmark ==="
	./benchmark$(TEST_EXT)
	@echo ""
	@echo "=== Running Feature Tests ==="
	./feature_test$(TEST_EXT)
	@echo ""
	@echo "=== Running Thread Tests ==="
	./thread_test$(TEST_EXT)

# ----- Clean -----
clean:
	$(RM) *.o *.a *.lib $(TARGET) benchmark$(TEST_EXT) feature_test$(TEST_EXT) thread_test$(TEST_EXT) performance_test$(TEST_EXT) 2>nul || true
	$(RM) test_*.txt test_*.log test_json_*.txt test_rot* 2>nul || true
