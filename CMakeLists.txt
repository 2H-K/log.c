cmake_minimum_required(VERSION 3.10)
project(logc VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler-specific flags
if(MSVC)
    add_compile_options(/W4 /WX- /std:c17)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Color output option
option(ENABLE_LOG_COLOR "Enable colored log output" ON)
if(ENABLE_LOG_COLOR)
    add_definitions(-DLOG_USE_COLOR)
endif()

# Source files
set(SOURCES src/log.c)
set(HEADERS src/log.h)

# Static library
add_library(logc STATIC ${SOURCES} ${HEADERS})
target_include_directories(logc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

# Threads: POSIX = pthread, Windows = Win32 threads (empty interface target)
find_package(Threads REQUIRED)
target_link_libraries(logc PUBLIC Threads::Threads)

# Test framework
add_library(test_harness STATIC tests/test_harness.c)
target_include_directories(test_harness PUBLIC tests)

# Tests
option(BUILD_TESTS "Build test suite" ON)
if(BUILD_TESTS)
    enable_testing()

    # Core tests (no threading)
    add_executable(test_core tests/core.c)
    target_link_libraries(test_core PRIVATE logc test_harness)
    add_test(NAME test_core COMMAND test_core)

    # Thread safety tests
    add_executable(test_thread tests/thread.c)
    target_link_libraries(test_thread PRIVATE logc test_harness Threads::Threads)
    add_test(NAME test_thread COMMAND test_thread)

    # Stress tests
    add_executable(test_stress tests/stress.c)
    target_link_libraries(test_stress PRIVATE logc test_harness Threads::Threads)
    add_test(NAME test_stress COMMAND test_stress)

    # Real malloc counting for the stress suite (GNU ld --wrap, Linux only)
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(test_stress PRIVATE LOG_TEST_WRAP_MALLOC)
        target_link_options(test_stress PRIVATE -Wl,--wrap=malloc)
    endif()

    # Platform tests
    add_executable(test_platform tests/platform.c)
    target_link_libraries(test_platform PRIVATE logc test_harness Threads::Threads)
    add_test(NAME test_platform COMMAND test_platform)

    # Performance benchmarks
    add_executable(test_perf tests/perf.c)
    target_link_libraries(test_perf PRIVATE logc test_harness Threads::Threads)
    add_test(NAME test_perf COMMAND test_perf)
endif()

# MSVC public header guard: /Oi (implied by /O2) makes `log` a builtin function
if(MSVC)
    add_executable(msvc_oi_smoke tests/compile/msvc_oi_smoke.c)
    target_link_libraries(msvc_oi_smoke PRIVATE logc)
    target_compile_options(msvc_oi_smoke PRIVATE /WX)
endif()

# Static-allocation mode tests: log.c compiled with LOG_STATIC_ALLOC and
# real allocator wrapping (A3 roadmap). Linux + GNU toolchain only.
if(BUILD_TESTS AND CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_static_alloc tests/static_alloc_main.c src/log.c)
    target_include_directories(test_static_alloc PRIVATE src tests)
    target_compile_definitions(test_static_alloc PRIVATE LOG_STATIC_ALLOC)
    target_link_libraries(test_static_alloc PRIVATE Threads::Threads)
    target_link_options(test_static_alloc PRIVATE
        -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc)
    add_test(NAME test_static_alloc COMMAND test_static_alloc)
endif()

# Examples
option(BUILD_EXAMPLES "Build example programs" ON)
if(BUILD_EXAMPLES)
    add_executable(example tests/example.c)
    target_link_libraries(example PRIVATE logc)
endif()

# Installation
install(TARGETS logc
    EXPORT logc-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

install(FILES src/log.h DESTINATION include)

install(EXPORT logc-targets
    FILE logc-config.cmake
    NAMESPACE logc::
    DESTINATION lib/cmake/logc
)
