# Copyright 2026 Swadhin Goswami
# Licensed under the Apache License, Version 2.0. See LICENSE.

include(FetchContent)
include(GoogleTest)

find_package(Threads REQUIRED)

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    message(STATUS "GoogleTest not found locally; fetching v1.14.0")
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)
endif()

# Test-only helpers, header only.
add_library(bc_test_support INTERFACE)
target_include_directories(bc_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

function(bc_add_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE
        bc_core
        bc_test_support
        GTest::gtest
        GTest::gtest_main
        Threads::Threads
    )
    bc_set_project_warnings(${target})
    gtest_discover_tests(${target})
endfunction()

bc_add_test(bc_unit_tests unit/test_bitmap.cpp)
bc_add_test(bc_history_tests unit/test_history.cpp)
bc_add_test(bc_concurrency_tests concurrency/test_generation.cpp)
