cmake_minimum_required(VERSION 3.28)

# The engine version (SemVer). main always carries the next version with the "-dev" suffix;
# a release commit removes the suffix (ADR-0002).
project(Ironthorn
    VERSION 0.1.0
    DESCRIPTION "Ironthorn - a modular 3D-first game engine"
    LANGUAGES C CXX)
set(IRONTHORN_VERSION_SUFFIX "-dev")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported. Use a preset: cmake --preset <name>")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(IRONTHORN_BUILD_TESTS "Build the unit and integration tests" ON)
option(IRONTHORN_BUILD_TOOLS "Build the tools-only modules, such as the shader compiler (downloads Slang and DXC)" ON)
option(IRONTHORN_WARNINGS_AS_ERRORS "Treat warnings in Ironthorn targets as errors" OFF)
option(IRONTHORN_ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer (Clang/GCC)" OFF)
option(IRONTHORN_ENABLE_TSAN "Build with ThreadSanitizer (Clang/GCC, not with IRONTHORN_ENABLE_SANITIZERS)" OFF)

# Sanitizer builds leave the tools out: their prebuilt libraries are not instrumented, so only
# the thin wrapper around them would be checked (docs/development/building.md).
if(IRONTHORN_BUILD_TOOLS AND (IRONTHORN_ENABLE_SANITIZERS OR IRONTHORN_ENABLE_TSAN))
    message(STATUS "Sanitizers are on: building without the tools (IRONTHORN_BUILD_TOOLS=OFF).")
    set(IRONTHORN_BUILD_TOOLS OFF)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The engine does not use C++20 modules (ADR-0001); scanning for them only slows the build.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Static C runtime, so shipped games run without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(IronthornCompilerOptions)
include(IronthornDependencies)
include(IronthornModule)

if(IRONTHORN_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(engine)

if(IRONTHORN_BUILD_TESTS)
    add_subdirectory(tests)
    ith_add_layering_test()

    add_test(NAME changelog_fragments
        COMMAND ${CMAKE_COMMAND} "-DREPOSITORY_DIR=${PROJECT_SOURCE_DIR}"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckChangelog.cmake")
    set_tests_properties(changelog_fragments PROPERTIES LABELS "lint")
endif()
