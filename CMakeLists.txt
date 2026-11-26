cmake_minimum_required(VERSION 3.28)

# The engine version (SemVer). main always carries the next version with the "-dev" suffix;
# a release commit removes the suffix (ADR-0002).
project(Ironthorn
    VERSION 0.2.0
    DESCRIPTION "Ironthorn - a modular 3D-first game engine"
    LANGUAGES C CXX)
set(IRONTHORN_VERSION_SUFFIX "-dev")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported. Use a preset: cmake --preset <name>")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(IRONTHORN_BUILD_TESTS "Build the unit and integration tests" ON)
# Module kinds (ADR-0017): runtime modules are always built; these options build the others.
option(IRONTHORN_BUILD_CLIENT "Build the client modules, which a dedicated server doesn't run (window, renderer, audio, game UI)" ON)
option(IRONTHORN_BUILD_TOOLS "Build the tools-only modules, such as the shader compiler (downloads Slang and DXC)" ON)
option(IRONTHORN_BUILD_EDITOR "Build the editor modules and applications (needs the client and tools modules)" ON)
option(IRONTHORN_WARNINGS_AS_ERRORS "Treat warnings in Ironthorn targets as errors" OFF)
option(IRONTHORN_ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer (Clang/GCC)" OFF)
option(IRONTHORN_ENABLE_TSAN "Build with ThreadSanitizer (Clang/GCC, not with IRONTHORN_ENABLE_SANITIZERS)" OFF)
# ADR-0007 rule 13: builds the Tracy profiler client in and turns the ITH_PROFILE_* macros on (it
# defines ITH_ENABLE_PROFILING). The profile presets set it. Never on in a build that is distributed.
option(IRONTHORN_PROFILING "Build with the Tracy profiler client (on demand, localhost only)" OFF)
# With IRONTHORN_PROFILING: whether the client collects only while a viewer is connected
# (TRACY_ON_DEMAND). OFF makes every zone active without a viewer, with the data queued in memory
# until one connects: for CI and short test runs, so that the GPU zones' timestamps, query resets
# and readbacks run under the validation layers (docs/development/logging.md).
option(IRONTHORN_PROFILING_ON_DEMAND "With IRONTHORN_PROFILING, collect only while a viewer is connected" ON)

# The shader compiler (Slang and DXC) is the one part of the tools that sanitizer builds leave
# out: its prebuilt libraries are not instrumented, so only the thin wrapper around them would be
# checked. Everything that needs it - the shader_compiler module, its tests, the shaders samples
# and tests embed at build time, scene_player - is built only when ITH_BUILD_SHADER_COMPILER is
# on. The other tools modules are built and checked (docs/development/building.md).
if(IRONTHORN_BUILD_TOOLS AND NOT (IRONTHORN_ENABLE_SANITIZERS OR IRONTHORN_ENABLE_TSAN))
    set(ITH_BUILD_SHADER_COMPILER ON)
else()
    set(ITH_BUILD_SHADER_COMPILER OFF)
    if(IRONTHORN_BUILD_TOOLS)
        message(STATUS "Sanitizers are on: building the tools without the shader compiler.")
    endif()
endif()

# Editor modules may depend on every kind, so they are built only with the client and tools. The
# editor application also needs the shader compiler, which sanitizer builds leave out; its modules
# don't, so editor_core and its headless tests are still built and checked there (ADR-0029 rule 19).
if(IRONTHORN_BUILD_EDITOR AND NOT (IRONTHORN_BUILD_CLIENT AND IRONTHORN_BUILD_TOOLS))
    message(STATUS "The client or tools modules are off: building without the editor (IRONTHORN_BUILD_EDITOR=OFF).")
    set(IRONTHORN_BUILD_EDITOR OFF)
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
add_subdirectory(tools)
add_subdirectory(samples)
add_subdirectory(editor)

if(IRONTHORN_BUILD_TESTS)
    add_subdirectory(tests)
    ith_add_layering_test()

    add_test(NAME changelog_fragments
        COMMAND ${CMAKE_COMMAND} "-DREPOSITORY_DIR=${PROJECT_SOURCE_DIR}"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckChangelog.cmake")
    set_tests_properties(changelog_fragments PROPERTIES LABELS "lint")

    # Engine code includes glm only through ith/math headers (ADR-0006 rule 2). tests/layering
    # holds lint fixtures that exist to be refused.
    add_test(NAME glm_includes
        COMMAND ${CMAKE_COMMAND} "-DREPOSITORY_DIR=${PROJECT_SOURCE_DIR}" "-DSKIP=tests/layering"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckGlmIncludes.cmake")
    set_tests_properties(glm_includes PROPERTIES LABELS "lint")

    # Dear ImGui stays inside editor_ui: only its sources include ImGui, only DockLayout.cpp its
    # internals, and only its CMakeLists.txt links it (ADR-0029 rule 2).
    add_test(NAME imgui_confined
        COMMAND ${CMAKE_COMMAND} "-DREPOSITORY_DIR=${PROJECT_SOURCE_DIR}"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckImGuiConfined.cmake")
    set_tests_properties(imgui_confined PROPERTIES LABELS "lint")

    # Tracy stays behind ith/foundation/Profile.h and the GPU-zone sources of the RHI backends: only
    # they include Tracy, and only foundation links it (ADR-0007 rules 10 to 12). tests/layering
    # holds lint fixtures that exist to be refused.
    add_test(NAME tracy_confined
        COMMAND ${CMAKE_COMMAND} "-DREPOSITORY_DIR=${PROJECT_SOURCE_DIR}" "-DSKIP=tests/layering"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckTracyConfined.cmake")
    set_tests_properties(tracy_confined PROPERTIES LABELS "lint")

    # File I/O under engine/ith goes through the file_system module (ADR-0035 rule 10).
    add_test(NAME file_io_confined
        COMMAND ${CMAKE_COMMAND} "-DREPOSITORY_DIR=${PROJECT_SOURCE_DIR}"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckFileIo.cmake")
    set_tests_properties(file_io_confined PROPERTIES LABELS "lint")
endif()
