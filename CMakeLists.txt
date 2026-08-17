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
set(SOURCES
    src/log.c
)

# Header files
set(HEADERS
    src/log.h
)

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

# Tests
option(BUILD_TESTS "Build test suite" ON)
if(BUILD_TESTS)
    enable_testing()

    add_executable(benchmark tests/benchmark.c)
    target_link_libraries(benchmark PRIVATE logc)
    add_test(NAME benchmark COMMAND benchmark)

    add_executable(feature_test tests/feature_test.c)
    target_link_libraries(feature_test PRIVATE logc)
    add_test(NAME feature_test COMMAND feature_test)

    add_executable(thread_test tests/thread_test.c)
    target_link_libraries(thread_test PRIVATE logc)
    add_test(NAME thread_test COMMAND thread_test)
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
