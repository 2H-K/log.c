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
# If user didn't set CC explicitly, auto-detect
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
  CFLAGS  += -std=c11 -Wall -Wextra -I src
  TEST_EXT =
else
  # MSVC cl.exe path
  LIB_PFX  =
  LIB_EXT  = .lib
  AR       = lib
  TARGET   = logc.lib
  LDFLAGS  =
  CFLAGS  += /std:c11 /W4 /WX- /D_CRT_SECURE_NO_WARNINGS /I src
  TEST_EXT = .exe
endif

# ----- Default: build library only -----
.PHONY: all lib test test_suite tsan clean

all: $(TARGET)

# ----- Static library -----
$(TARGET): src/log.c src/log.h
	$(CC) $(CFLAGS) -c src/log.c -o log.o
	$(AR) rcs $@ log.o

# ----- Test executables -----
# NOTE: the binary is named test_bug (not test) so it does not collide
# with the test/ source directory on POSIX systems.
test: test_bug$(TEST_EXT)

test_bug$(TEST_EXT): test/test_bug.c src/log.c src/log.h
	$(CC) $(CFLAGS) -o $@ test/test_bug.c src/log.c $(LDFLAGS)

test_suite: test_suite$(TEST_EXT)

test_suite$(TEST_EXT): test/test_suite.c src/log.c src/log.h
	$(CC) $(CFLAGS) -o $@ test/test_suite.c src/log.c $(LDFLAGS)

# ThreadSanitizer build: detects data races in multithreaded tests
tsan: test/test_suite.c src/log.c src/log.h
	$(CC) $(CFLAGS) -fsanitize=thread -g -O1 -o tsan_test test/test_suite.c src/log.c $(LDFLAGS)
	./tsan_test

# ----- Clean -----
clean:
	$(RM) *.o *.a *.lib $(TARGET) test_bug$(TEST_EXT) test_suite$(TEST_EXT) tsan_test 2>nul || true
