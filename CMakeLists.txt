cmake_minimum_required(VERSION 3.10)
project(logc VERSION 2.0.0 LANGUAGES C)

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

# Platform-specific libraries
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(logc PUBLIC Threads::Threads)
endif()

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

    # Platform tests
    add_executable(test_platform tests/platform.c)
    target_link_libraries(test_platform PRIVATE logc test_harness Threads::Threads)
    add_test(NAME test_platform COMMAND test_platform)

    # Performance benchmarks
    add_executable(test_perf tests/perf.c)
    target_link_libraries(test_perf PRIVATE logc test_harness Threads::Threads)
    add_test(NAME test_perf COMMAND test_perf)
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
