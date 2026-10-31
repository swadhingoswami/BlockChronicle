# Copyright 2026 Swadhin Goswami
# Licensed under the Apache License, Version 2.0. See LICENSE.

cmake_minimum_required(VERSION 3.20)
project(BlockChronicle VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(BC_BUILD_TESTS "Build unit tests" ON)
set(BC_SANITIZE "" CACHE STRING
    "Sanitizers to enable, e.g. 'address,undefined' or 'thread'. Empty disables.")

if(BC_SANITIZE)
    add_compile_options(-fsanitize=${BC_SANITIZE} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${BC_SANITIZE})
endif()

# Warnings are errors for the project's own targets. Tests are held to the same
# standard: a test that does not compile cleanly is not evidence of anything.
set(BC_WARNING_FLAGS "")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(BC_WARNING_FLAGS
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow)
endif()

function(bc_set_project_warnings target)
    if(BC_WARNING_FLAGS)
        target_compile_options(${target} PRIVATE ${BC_WARNING_FLAGS})
    endif()
endfunction()

add_library(bc_core STATIC
    src/core/bitmap.cpp
    src/core/generation.cpp
    src/core/history.cpp
)
add_library(BlockChronicle::core ALIAS bc_core)
target_include_directories(bc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
bc_set_project_warnings(bc_core)

# Phase 1 has no Linux-specific code by design: Bitmap and Geometry are
# platform-neutral so they can be developed and tested on any host. Linux code
# appears at Phase 7 behind the TrackingBackend interface.
if(BC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
