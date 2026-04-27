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
  TEST_EXT = .exe
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
.PHONY: all lib test test_fixes clean

all: $(TARGET)

# ----- Static library -----
$(TARGET): src/log.c src/log.h
	$(CC) $(CFLAGS) -c src/log.c -o log.o
	$(AR) rcs $@ log.o

# ----- Test executable -----
test: test$(TEST_EXT)

test$(TEST_EXT): test/test_bug.c src/log.c src/log.h
	$(CC) $(CFLAGS) -o $@ test/test_bug.c src/log.c $(LDFLAGS)

# ----- Clean -----
clean:
	$(RM) *.o *.a *.lib $(TARGET) test$(TEST_EXT) test_fixes$(TEST_EXT) 2>nul || true
