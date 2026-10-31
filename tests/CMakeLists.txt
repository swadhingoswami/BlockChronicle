# Copyright 2026 Swadhin Goswami
# Licensed under the Apache License, Version 2.0. See LICENSE.

include(FetchContent)

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

add_executable(bc_unit_tests
    unit/test_bitmap.cpp
)

target_link_libraries(bc_unit_tests PRIVATE
    bc_core
    GTest::gtest
    GTest::gtest_main
)

bc_set_project_warnings(bc_unit_tests)

include(GoogleTest)
gtest_discover_tests(bc_unit_tests)
