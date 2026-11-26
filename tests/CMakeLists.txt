# Shared test support: doctest's main and helpers such as AssertCapture.
add_library(ith_test_support STATIC
    support/TestMain.cpp
    support/AssertCapture.h
    support/AssertCapture.cpp)
add_library(ith::test_support ALIAS ith_test_support)
target_include_directories(ith_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(ith_test_support PUBLIC ith::build_options ith::foundation doctest::doctest)
# Tests are built without exceptions like the engine. doctest can't leave a test case then: a
# failed REQUIRE or FAIL is recorded like a CHECK and the test case continues (TestMain.cpp).
target_compile_definitions(ith_test_support PUBLIC DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
set_target_properties(ith_test_support PROPERTIES
    FOLDER "Ironthorn/Tests"
    COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})

# std::thread in the tests needs the platform thread library on Linux.
find_package(Threads REQUIRED)

ith_add_test_suite(foundation
    SOURCES
        foundation/AssertTests.cpp
        foundation/ExpectedTests.cpp
        foundation/FixedTimestepTests.cpp
        foundation/HashTests.cpp
        foundation/LinearAllocatorTests.cpp
        foundation/LogRingBufferTests.cpp
        foundation/LogStrippingTests.cpp
        foundation/LogTests.cpp
        foundation/MutexTests.cpp
        foundation/ObjectPoolTests.cpp
        foundation/ProfileTests.cpp
        foundation/StopwatchTests.cpp
        foundation/StringUtilsTests.cpp
        foundation/UuidTests.cpp
        foundation/VersionTests.cpp
    LIBRARIES ith::foundation Threads::Threads)

# A fatal error ends the process, so these test cases are skipped in the suite and run alone here,
# passing when the Fatal record reaches an installed sink and stderr (ADR-0007 rule 9), also when
# the stderr sink was removed and when nothing had used the logger before.
add_test(NAME log_fatal_reaches_sinks
    COMMAND ith_foundation_tests "--test-case=Log: a fatal error reaches the sinks and stderr" --no-skip
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
set_tests_properties(log_fatal_reaches_sinks PROPERTIES
    LABELS "unit"
    TIMEOUT 60
    PASS_REGULAR_EXPRESSION "sink received Fatal: fatal error: logger fatal test.*\\[Fatal\\] Assert: fatal error: logger fatal test")
add_test(NAME log_fatal_before_first_use
    COMMAND ith_foundation_tests "--test-case=Log: a fatal error before the logger was used*" --no-skip
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
set_tests_properties(log_fatal_before_first_use PROPERTIES
    LABELS "unit"
    TIMEOUT 60
    PASS_REGULAR_EXPRESSION "\\[Fatal\\] Assert: fatal error: fatal before the logger was used")

# The logger benchmark (docs/development/logging.md): disabled statements, formatted records and
# contention. Like jobs_benchmark it only prints numbers.
add_executable(ith_log_benchmark foundation/LogBenchmark.cpp)
target_link_libraries(ith_log_benchmark PRIVATE ith::foundation Threads::Threads)
set_target_properties(ith_log_benchmark PROPERTIES
    FOLDER "Ironthorn/Benchmarks"
    COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
add_test(NAME log_benchmark COMMAND ith_log_benchmark)
set_tests_properties(log_benchmark PROPERTIES LABELS "benchmark" TIMEOUT 600)

if(ITH_BUILD_SHADER_COMPILER)
    ith_add_test_suite(shader_compiler
        SOURCES shader_compiler/ShaderCompilerTests.cpp
        LIBRARIES ith::shader_compiler)
    ith_copy_shader_compiler_runtime(ith_shader_compiler_tests)
endif()

# Input: the stable control ids and names (which must never change), state, routing,
# consumption, focus, capture and assignment, with injected events.
ith_add_test_suite(input
    SOURCES
        input/ControlNamesTests.cpp
        input/InputStateTests.cpp
        input/InputTests.cpp
    LIBRARIES ith::input)

# The input action layer (docs/specs/input-actions.md): assets and validation, values, modifiers,
# triggers, phases and events, contexts and claims, routing and rebinding, with injected events and
# explicit frame times; RegressionTests.cpp holds the cases found in review.
ith_add_test_suite(input_actions
    SOURCES
        input_actions/ActionTestSupport.h
        input_actions/AssetTests.cpp
        input_actions/BindingTests.cpp
        input_actions/ContextTests.cpp
        input_actions/RegressionTests.cpp
        input_actions/TriggerTests.cpp
        input_actions/WorldCopyTests.cpp
    LIBRARIES ith::input_actions)

# No allocation in steady state: an executable of its own, because AllocationTests.cpp replaces the
# global allocation functions with counting ones. Sanitizer builds replace them too (ASan reports
# mismatched pairs, TSan's runtime defines them), so it isn't built there.
if(NOT IRONTHORN_ENABLE_SANITIZERS AND NOT IRONTHORN_ENABLE_TSAN)
    ith_add_test_suite(input_actions_allocation
        SOURCES
            input_actions/ActionTestSupport.h
            input_actions/AllocationTests.cpp
        LIBRARIES ith::input_actions)
endif()

# A libFuzzer target for the untrusted readers of the input action layer and the serialization
# envelope: readDocument, asset documents with their id, the input action and context assets and
# the bindings document, in Clang sanitizer builds only, with a short smoke run like the others.
if(IRONTHORN_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"
        AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    add_executable(ith_input_actions_fuzz input_actions/InputDocumentFuzz.cpp)
    target_link_libraries(ith_input_actions_fuzz PRIVATE ith::input_actions)
    target_compile_options(ith_input_actions_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(ith_input_actions_fuzz PRIVATE -fsanitize=fuzzer)
    set_target_properties(ith_input_actions_fuzz PROPERTIES
        FOLDER "Ironthorn/Tests"
        COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
    set(corpus "${CMAKE_CURRENT_BINARY_DIR}/input_actions_fuzz_corpus")
    file(MAKE_DIRECTORY "${corpus}")
    add_test(NAME input_actions_fuzz
        COMMAND ith_input_actions_fuzz -runs=20000 -max_len=4096 -malloc_limit_mb=512
            -dict=${CMAKE_CURRENT_SOURCE_DIR}/serialization/fuzz_seeds/json.dict
            "${corpus}" "${CMAKE_CURRENT_SOURCE_DIR}/input_actions/fuzz_seeds")
    set_tests_properties(input_actions_fuzz PROPERTIES LABELS "fuzz" TIMEOUT 300)
endif()

# The input actions benchmark (docs/development/input-actions.md), like the ECS benchmark: it only
# prints numbers, and its "benchmark" label keeps it out of normal test runs.
add_executable(ith_input_actions_benchmark input_actions/InputActionsBenchmark.cpp)
target_link_libraries(ith_input_actions_benchmark PRIVATE ith::input_actions)
set_target_properties(ith_input_actions_benchmark PROPERTIES
    FOLDER "Ironthorn/Benchmarks"
    COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
add_test(NAME input_actions_benchmark COMMAND ith_input_actions_benchmark)
set_tests_properties(input_actions_benchmark PROPERTIES LABELS "benchmark" TIMEOUT 600)

# The platform layer on SDL's headless "offscreen" driver. The tests also call SDL directly, to
# list its compiled-in video drivers and to push events the offscreen driver can't produce.
# WakeTests.cpp checks waitEvents and Platform::wake with a JobSystem (ADR-0035 rule 23).
if(IRONTHORN_BUILD_CLIENT)
    ith_add_test_suite(platform
        SOURCES
            platform/PlatformTests.cpp
            platform/WakeTests.cpp
        LIBRARIES ith::platform ith::jobs SDL3::SDL3-static Threads::Threads)
endif()

# The RHI on its null backend: handles, deferred destruction, frame pacing, swapchains, device
# loss, the recording checks and the format queries, without a GPU.
if(IRONTHORN_BUILD_CLIENT)
    ith_add_test_suite(rhi
        SOURCES
            rhi/CommandListTests.cpp
            rhi/ConcurrencyTests.cpp
            rhi/DeviceTests.cpp
            rhi/FormatTests.cpp
            rhi/FrameTests.cpp
            rhi/RhiTestSupport.h
        LIBRARIES ith::rhi Threads::Threads)
endif()

# Backend selection, on every platform that builds client modules, without a GPU.
if(IRONTHORN_BUILD_CLIENT)
    ith_add_test_suite(rhi_select
        SOURCES rhi_select/CreateDeviceTests.cpp
        LIBRARIES ith::rhi_select)
endif()

# The Direct3D 12 backend on a GPU (Windows only): WARP by default, so it runs on hosted CI
# runners, or the fastest hardware GPU with ITH_RHI_TEST_ADAPTER=hardware. With the shader compiler built,
# its shaders are compiled from Slang at run time and the pipeline tests run too.
if(TARGET ith_rhi_d3d12)
    set(ith_rhi_d3d12_test_libraries ith::rhi_d3d12 ith::rhi_select Threads::Threads)
    if(ITH_BUILD_SHADER_COMPILER)
        list(APPEND ith_rhi_d3d12_test_libraries ith::shader_compiler)
    endif()
    ith_add_test_suite(rhi_d3d12
        SOURCES rhi_d3d12/D3D12DeviceTests.cpp
        LIBRARIES ${ith_rhi_d3d12_test_libraries}
        LABELS gpu)
    ith_use_d3d12_agility_sdk(ith_rhi_d3d12_tests)
    if(ITH_BUILD_SHADER_COMPILER)
        target_compile_definitions(ith_rhi_d3d12_tests PRIVATE ITH_TEST_HAS_SHADER_COMPILER=1)
        ith_copy_shader_compiler_runtime(ith_rhi_d3d12_tests)
    endif()
endif()

# The Vulkan backend on a GPU (Windows and Linux): the fastest hardware GPU, or a software one such
# as lavapipe. Without a Vulkan loader or driver every test reports itself skipped, unless
# ITH_RHI_TEST_REQUIRE_VULKAN=1. With the shader compiler built, its shaders are compiled from Slang at run
# time and the pipeline tests run too.
if(TARGET ith_rhi_vulkan)
    set(ith_rhi_vulkan_test_libraries ith::rhi_vulkan ith::rhi_select Threads::Threads)
    if(ITH_BUILD_SHADER_COMPILER)
        list(APPEND ith_rhi_vulkan_test_libraries ith::shader_compiler)
    endif()
    ith_add_test_suite(rhi_vulkan
        SOURCES rhi_vulkan/VulkanDeviceTests.cpp
        LIBRARIES ${ith_rhi_vulkan_test_libraries}
        LABELS gpu)
    if(ITH_BUILD_SHADER_COMPILER)
        target_compile_definitions(ith_rhi_vulkan_tests PRIVATE ITH_TEST_HAS_SHADER_COMPILER=1)
        ith_copy_shader_compiler_runtime(ith_rhi_vulkan_tests)
    endif()
endif()

# scene_player's scenes (the triangle and 10,000 instanced cubes) rendered offscreen on every GPU
# backend the machine has, and checked analytically. Its shaders come from slangc at build time,
# so it exists only with the tools, like the sample.
if(TARGET scene_player)
    ith_add_test_suite(scene_player
        SOURCES scene_player/SceneTests.cpp "${PROJECT_SOURCE_DIR}/samples/scene_player/Scenes.cpp"
        LIBRARIES ith::math ith::rhi ith::rhi_select ith::render_graph
        LABELS gpu)
    target_include_directories(ith_scene_player_tests PRIVATE "${PROJECT_SOURCE_DIR}/samples/scene_player")
    ith_scene_player_shaders(ith_scene_player_tests)
    if(TARGET ith_rhi_d3d12)
        ith_use_d3d12_agility_sdk(ith_scene_player_tests)
    endif()
endif()

# The render graph (ADR-0028): compile tests through the text dump, without a device; execute
# tests on the null backend (groups, serial and parallel recording, the pool across frames,
# imports, clobbering, out of memory); and the validation errors.
if(IRONTHORN_BUILD_CLIENT)
    ith_add_test_suite(render_graph
        SOURCES
            render_graph/CompileTests.cpp
            render_graph/ExecuteTests.cpp
            render_graph/RenderGraphTestSupport.h
            render_graph/ValidationTests.cpp
        LIBRARIES ith::render_graph Threads::Threads)

    # No allocation when declaring and compiling a steady frame: an executable of its own, because
    # AllocationTests.cpp replaces the global allocation functions with counting ones. Sanitizer
    # builds replace them too, so it isn't built there.
    if(NOT IRONTHORN_ENABLE_SANITIZERS AND NOT IRONTHORN_ENABLE_TSAN)
        ith_add_test_suite(render_graph_allocation
            SOURCES render_graph/AllocationTests.cpp
            LIBRARIES ith::render_graph)
    endif()

    # The render graph benchmark (docs/development/render-graph.md): declare, compile and execute
    # times of 100 passes and 200 resources. Like ecs_benchmark it only prints numbers.
    add_executable(ith_render_graph_benchmark render_graph/RenderGraphBenchmark.cpp)
    target_link_libraries(ith_render_graph_benchmark PRIVATE ith::render_graph)
    set_target_properties(ith_render_graph_benchmark PROPERTIES
        FOLDER "Ironthorn/Benchmarks"
        COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
    add_test(NAME render_graph_benchmark COMMAND ith_render_graph_benchmark)
    set_tests_properties(render_graph_benchmark PROPERTIES LABELS "benchmark" TIMEOUT 600)
endif()

# The render graph on every GPU backend the machine has, with validation (synchronization
# validation on Vulkan, GPU-based validation on Direct3D 12): depth and HDR color, a tonemap, a
# history texture over three frames, transient reuse, clobbering, parallel recording and a resize
# that evicts pool entries. Its shaders come from slangc at build time, so it exists only with the
# tools, like scene_player's test.
if(IRONTHORN_BUILD_CLIENT AND ITH_BUILD_SHADER_COMPILER)
    ith_add_test_suite(render_graph_gpu
        SOURCES render_graph_gpu/RenderGraphGpuTests.cpp
        LIBRARIES ith::render_graph ith::rhi_select Threads::Threads
        LABELS gpu)
    set(ith_render_graph_dxil "")
    if(TARGET ith_rhi_d3d12)
        set(ith_render_graph_dxil DXIL)
        target_compile_definitions(ith_render_graph_gpu_tests PRIVATE ITH_RENDER_GRAPH_TEST_HAS_DXIL=1)
        ith_use_d3d12_agility_sdk(ith_render_graph_gpu_tests)
    endif()
    ith_embed_shaders(ith_render_graph_gpu_tests SOURCE render_graph_gpu/RenderGraphTest.slang
        ENTRY_POINTS fullscreenVertex:vertex fillPixel:pixel combinePixel:pixel tonemapPixel:pixel
        ${ith_render_graph_dxil})
endif()

# A libFuzzer target for the Vulkan backend's SPIR-V binding check, in Clang sanitizer builds
# only, with a short smoke run in CTest like the others, seeded with SPIR-V the shader compiler
# wrote (rhi_vulkan/fuzz_seeds).
if(TARGET ith_rhi_vulkan AND IRONTHORN_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"
        AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    add_executable(ith_rhi_vulkan_spirv_fuzz rhi_vulkan/SpirvCheckFuzz.cpp)
    target_link_libraries(ith_rhi_vulkan_spirv_fuzz PRIVATE ith::rhi_vulkan)
    target_compile_options(ith_rhi_vulkan_spirv_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(ith_rhi_vulkan_spirv_fuzz PRIVATE -fsanitize=fuzzer)
    set_target_properties(ith_rhi_vulkan_spirv_fuzz PROPERTIES
        FOLDER "Ironthorn/Tests"
        COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
    set(corpus "${CMAKE_CURRENT_BINARY_DIR}/rhi_vulkan_spirv_fuzz_corpus")
    file(MAKE_DIRECTORY "${corpus}")
    add_test(NAME rhi_vulkan_spirv_fuzz
        COMMAND ith_rhi_vulkan_spirv_fuzz -runs=20000 -max_len=8192 -malloc_limit_mb=512
            "${corpus}" "${CMAKE_CURRENT_SOURCE_DIR}/rhi_vulkan/fuzz_seeds")
    set_tests_properties(rhi_vulkan_spirv_fuzz PROPERTIES LABELS "fuzz" TIMEOUT 300)
endif()

# The reflection generator, run in-process on fixture headers.
ith_add_test_suite(reflect_tool
    SOURCES reflect_tool/ScannerTests.cpp
    LIBRARIES ith::reflect_scanner)

# A libFuzzer target for the generator, in Clang sanitizer builds only. CTest runs a short smoke
# run seeded with the fixture headers; new inputs go to a corpus in the build tree.
if(IRONTHORN_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"
        AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    add_executable(ith_reflect_fuzz reflect_tool/ScannerFuzz.cpp)
    target_link_libraries(ith_reflect_fuzz PRIVATE ith::reflect_scanner)
    target_compile_options(ith_reflect_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(ith_reflect_fuzz PRIVATE -fsanitize=fuzzer)
    set_target_properties(ith_reflect_fuzz PROPERTIES
        FOLDER "Ironthorn/Tests"
        COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
    set(corpus "${CMAKE_CURRENT_BINARY_DIR}/reflect_fuzz_corpus")
    file(MAKE_DIRECTORY "${corpus}")
    add_test(NAME reflect_tool_fuzz
        COMMAND ith_reflect_fuzz -runs=20000 -max_len=4096 "${corpus}" "${CMAKE_CURRENT_SOURCE_DIR}/reflect_tool/fixtures")
    set_tests_properties(reflect_tool_fuzz PROPERTIES LABELS "fuzz" TIMEOUT 300)
endif()

# Reflected test types: their records are generated by the build, as for an engine module.
add_library(ith_test_reflected STATIC)
target_include_directories(ith_test_reflected PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/reflection")
target_link_libraries(ith_test_reflected
    PUBLIC ith::build_options ith::reflection
    PRIVATE ith::runtime_options)
set_target_properties(ith_test_reflected PROPERTIES
    FOLDER "Ironthorn/Tests"
    COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
ith_reflect_target(ith_test_reflected
    MODULE test_reflected
    KIND runtime
    INCLUDE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/reflection"
    HEADERS
        reflection/ReflectedTypes.h
        reflection/Loadout.h)

ith_add_test_suite(reflection
    SOURCES
        reflection/EntitiesTests.cpp
        reflection/PatchTests.cpp
        reflection/PathTestSupport.h
        reflection/PropertyPathTests.cpp
        reflection/RoundTripTests.cpp
        reflection/TypeOfTests.cpp
        reflection/TypeRegistryTests.cpp
    LIBRARIES ith_test_reflected ith_test_archive)

# A libFuzzer target for the property path parser, in Clang sanitizer builds only, with a short
# smoke run in CTest like the others, seeded with sample paths (reflection/fuzz_seeds).
if(IRONTHORN_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"
        AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    add_executable(ith_reflection_path_fuzz reflection/PropertyPathFuzz.cpp)
    target_link_libraries(ith_reflection_path_fuzz PRIVATE ith::reflection ith_test_reflected ith_test_archive)
    target_compile_options(ith_reflection_path_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(ith_reflection_path_fuzz PRIVATE -fsanitize=fuzzer)
    set_target_properties(ith_reflection_path_fuzz PROPERTIES
        FOLDER "Ironthorn/Tests"
        COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
    set(corpus "${CMAKE_CURRENT_BINARY_DIR}/reflection_path_fuzz_corpus")
    file(MAKE_DIRECTORY "${corpus}")
    add_test(NAME reflection_path_fuzz
        COMMAND ith_reflection_path_fuzz -runs=20000 -max_len=256 "${corpus}"
            "${CMAKE_CURRENT_SOURCE_DIR}/reflection/fuzz_seeds")
    set_tests_properties(reflection_path_fuzz PROPERTIES LABELS "fuzz" TIMEOUT 300)
endif()

# A reflected test component with math fields, generated like an engine module's records.
add_library(ith_test_math_reflected STATIC)
target_include_directories(ith_test_math_reflected PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/math")
target_link_libraries(ith_test_math_reflected
    PUBLIC ith::build_options ith::reflection
    PRIVATE ith::runtime_options)
set_target_properties(ith_test_math_reflected PROPERTIES
    FOLDER "Ironthorn/Tests"
    COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
ith_reflect_target(ith_test_math_reflected
    MODULE test_math
    KIND runtime
    INCLUDE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/math"
    HEADERS math/MathComponents.h)

# Math (ADR-0006): the conventions pinned (glm's configuration, axes, matrix layout and order,
# quaternions, projections), bounds, frustums and rays, and the math types in reflection, the
# archives and the ECS.
ith_add_test_suite(math
    SOURCES
        math/ConventionTests.cpp
        math/GeometryTests.cpp
        math/ReflectionTests.cpp
    LIBRARIES ith::math ith::ecs ith::serialization ith_test_math_reflected)

# Reflected types for the serialization tests: a runtime module and an editor module, the only
# kind that may use EditorOnly.
add_library(ith_test_archive STATIC)
target_include_directories(ith_test_archive PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/serialization")
target_link_libraries(ith_test_archive
    PUBLIC ith::build_options ith::reflection
    PRIVATE ith::runtime_options)
set_target_properties(ith_test_archive PROPERTIES
    FOLDER "Ironthorn/Tests"
    COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
ith_reflect_target(ith_test_archive
    MODULE test_archive
    KIND runtime
    INCLUDE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/serialization"
    HEADERS serialization/ArchiveTypes.h)
ith_reflect_target(ith_test_archive
    MODULE test_archive_editor
    KIND editor
    INCLUDE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/serialization"
    HEADERS serialization/EditorArchiveTypes.h)

ith_add_test_suite(serialization
    SOURCES
        serialization/BinaryArchiveTests.cpp
        serialization/EntityMappingTests.cpp
        serialization/JsonTests.cpp
        serialization/RoundTripTests.cpp
        serialization/TextArchiveTests.cpp
        serialization/ValueTreeTests.cpp
    LIBRARIES ith::serialization ith_test_reflected ith_test_archive)

# libFuzzer targets for the text and binary readers, in Clang sanitizer builds only, with short
# smoke runs in CTest like reflect_tool_fuzz, seeded with sample documents and blobs
# (serialization/fuzz_seeds); the text target also gets a JSON dictionary.
if(IRONTHORN_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"
        AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    foreach(reader text binary)
        set(target ith_serialization_${reader}_fuzz)
        if(reader STREQUAL "text")
            add_executable(${target} serialization/TextReaderFuzz.cpp)
            set(options "-dict=${CMAKE_CURRENT_SOURCE_DIR}/serialization/fuzz_seeds/json.dict")
        else()
            add_executable(${target} serialization/BinaryReaderFuzz.cpp)
            set(options "")
        endif()
        target_link_libraries(${target} PRIVATE ith::serialization ith_test_reflected ith_test_archive)
        target_compile_options(${target} PRIVATE -fsanitize=fuzzer)
        target_link_options(${target} PRIVATE -fsanitize=fuzzer)
        set_target_properties(${target} PROPERTIES
            FOLDER "Ironthorn/Tests"
            COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
        set(corpus "${CMAKE_CURRENT_BINARY_DIR}/serialization_${reader}_fuzz_corpus")
        file(MAKE_DIRECTORY "${corpus}")
        add_test(NAME serialization_${reader}_fuzz
            COMMAND ${target} -runs=20000 -max_len=4096 -malloc_limit_mb=512 ${options}
                "${corpus}" "${CMAKE_CURRENT_SOURCE_DIR}/serialization/fuzz_seeds/${reader}")
        set_tests_properties(serialization_${reader}_fuzz PROPERTIES LABELS "fuzz" TIMEOUT 300)
    endforeach()
endif()

# Component types for the ECS tests and benchmark, reflected like an engine module's.
add_library(ith_test_ecs_components STATIC)
target_include_directories(ith_test_ecs_components PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ecs")
target_link_libraries(ith_test_ecs_components
    PUBLIC ith::build_options ith::ecs
    PRIVATE ith::runtime_options)
set_target_properties(ith_test_ecs_components PROPERTIES
    FOLDER "Ironthorn/Tests"
    COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
ith_reflect_target(ith_test_ecs_components
    MODULE test_ecs
    KIND runtime
    INCLUDE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ecs"
    HEADERS ecs/EcsComponents.h)
ith_reflect_target(ith_test_ecs_components
    MODULE test_ecs_editor
    KIND editor
    INCLUDE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ecs"
    HEADERS ecs/EditorEcsComponents.h)

ith_add_test_suite(ecs
    SOURCES
        ecs/ChangeDetectionTests.cpp
        ecs/CommandTests.cpp
        ecs/ConcurrencyTests.cpp
        ecs/DynamicAccessTests.cpp
        ecs/EntityOpTests.cpp
        ecs/EntityPatchTests.cpp
        ecs/HierarchyTests.cpp
        ecs/HookTests.cpp
        ecs/ModelTests.cpp
        ecs/ResourceTests.cpp
        ecs/ScheduleTests.cpp
        ecs/StableIdTests.cpp
        ecs/WorldCopyTests.cpp
        ecs/WorldTests.cpp
    LIBRARIES ith_test_ecs_components ith::jobs Threads::Threads)

# A libFuzzer target for the ECS paths that take type records (ADR-0014 rule 19), in Clang
# sanitizer builds only, with a short smoke run like the others. It needs no seeds: any input is
# a sequence of operations.
if(IRONTHORN_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"
        AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    add_executable(ith_ecs_dynamic_fuzz ecs/DynamicAccessFuzz.cpp)
    target_link_libraries(ith_ecs_dynamic_fuzz PRIVATE ith_test_ecs_components)
    target_compile_options(ith_ecs_dynamic_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(ith_ecs_dynamic_fuzz PRIVATE -fsanitize=fuzzer)
    set_target_properties(ith_ecs_dynamic_fuzz PROPERTIES
        FOLDER "Ironthorn/Tests"
        COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
    set(corpus "${CMAKE_CURRENT_BINARY_DIR}/ecs_dynamic_fuzz_corpus")
    file(MAKE_DIRECTORY "${corpus}")
    add_test(NAME ecs_dynamic_fuzz
        COMMAND ith_ecs_dynamic_fuzz -runs=20000 -max_len=4096 -malloc_limit_mb=512 "${corpus}")
    set_tests_properties(ecs_dynamic_fuzz PROPERTIES LABELS "fuzz" TIMEOUT 300)
endif()

# Component types for the level tests and fuzz target, reflected like an engine module's, with an
# editor module for the EditorOnly ones.
add_library(ith_test_level_components STATIC)
target_include_directories(ith_test_level_components PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/level")
target_link_libraries(ith_test_level_components
    PUBLIC ith::build_options ith::reflection ith::math
    PRIVATE ith::runtime_options)
set_target_properties(ith_test_level_components PROPERTIES
    FOLDER "Ironthorn/Tests"
    COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
ith_reflect_target(ith_test_level_components
    MODULE test_level
    KIND runtime
    INCLUDE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/level"
    HEADERS level/LevelComponents.h)
ith_reflect_target(ith_test_level_components
    MODULE test_level_editor
    KIND editor
    INCLUDE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/level"
    HEADERS level/EditorLevelComponents.h)

# Level documents (ADR-0034): layout and stable bytes, a 10,000-entity round trip, the hierarchy,
# references, validation errors, unresolved components, the entity mapping, hooks and stripped
# editor sections.
ith_add_test_suite(level
    SOURCES level/LevelTests.cpp
    LIBRARIES ith::level ith_test_level_components)

# Primitive shapes (ADR-0042 rule 19): bounds, mesh indices, outward normals and winding, the
# ShapeRenderer record and a level round trip.
ith_add_test_suite(shapes
    SOURCES shapes/ShapeTests.cpp
    LIBRARIES ith::shapes ith::level)

# libFuzzer targets for loadLevel, into fresh and filled worlds, and for override records fed into
# World::applyPatches, in Clang sanitizer builds only, with short smoke runs like the others,
# seeded with sample levels (level/fuzz_seeds) and records (level/override_seeds) and the level
# dictionary. -timeout=10 turns an input that takes too long into a failure.
if(IRONTHORN_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"
        AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    foreach(target level override)
        if(target STREQUAL "level")
            add_executable(ith_level_fuzz level/LevelLoadFuzz.cpp)
            set(seeds "${CMAKE_CURRENT_SOURCE_DIR}/level/fuzz_seeds")
        else()
            add_executable(ith_override_fuzz level/OverrideFuzz.cpp)
            set(seeds "${CMAKE_CURRENT_SOURCE_DIR}/level/override_seeds")
        endif()
        target_link_libraries(ith_${target}_fuzz PRIVATE ith::level ith_test_level_components)
        target_compile_options(ith_${target}_fuzz PRIVATE -fsanitize=fuzzer)
        target_link_options(ith_${target}_fuzz PRIVATE -fsanitize=fuzzer)
        set_target_properties(ith_${target}_fuzz PROPERTIES
            FOLDER "Ironthorn/Tests"
            COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
        set(corpus "${CMAKE_CURRENT_BINARY_DIR}/${target}_fuzz_corpus")
        file(MAKE_DIRECTORY "${corpus}")
        add_test(NAME ${target}_fuzz
            COMMAND ith_${target}_fuzz -runs=20000 -max_len=4096 -malloc_limit_mb=512 -timeout=10
                -dict=${CMAKE_CURRENT_SOURCE_DIR}/level/level.dict "${corpus}" "${seeds}")
        set_tests_properties(${target}_fuzz PROPERTIES LABELS "fuzz" TIMEOUT 300)
    endforeach()
endif()

# The ECS benchmark (docs/development/ecs.md). Its test only prints numbers and never fails on
# them; the "benchmark" label keeps it out of normal test runs, which exclude that label.
add_executable(ith_ecs_benchmark ecs/EcsBenchmark.cpp)
target_link_libraries(ith_ecs_benchmark PRIVATE ith_test_ecs_components)
set_target_properties(ith_ecs_benchmark PROPERTIES
    FOLDER "Ironthorn/Benchmarks"
    COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
add_test(NAME ecs_benchmark COMMAND ith_ecs_benchmark)
set_tests_properties(ecs_benchmark PROPERTIES LABELS "benchmark" TIMEOUT 600)

# The module_layering check must refuse what ADR-0004 and ADR-0017 forbid. Each case runs the
# check on a made-up module table (fixture sources in tests/layering) and passes when the
# expected violation is reported.
function(ith_add_layering_failure_test name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "MODULES;APPLICATIONS;UNAVAILABLE;EXPECT" "")
    string(REPLACE ";" "," layers "${ITH_LAYERS}")
    add_test(NAME module_layering_refuses_${name}
        COMMAND ${CMAKE_COMMAND}
            "-DREPOSITORY_DIR=${PROJECT_SOURCE_DIR}"
            "-DMODULE_TABLE=${arg_MODULES}"
            "-DAPPLICATION_TABLE=${arg_APPLICATIONS}"
            "-DLAYERS=${layers}"
            "-DUNAVAILABLE_MODULES=${arg_UNAVAILABLE}"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckModuleLayering.cmake")
    set_tests_properties(module_layering_refuses_${name} PROPERTIES
        LABELS "lint"
        PASS_REGULAR_EXPRESSION "${arg_EXPECT}")
endfunction()

# The glm_includes check must refuse a direct glm include outside engine/ith/math (ADR-0006 rule
# 2); the fixture includes glm and is never compiled.
add_test(NAME glm_includes_refuses_direct_include
    COMMAND ${CMAKE_COMMAND} "-DREPOSITORY_DIR=${PROJECT_SOURCE_DIR}" "-DDIRECTORIES=tests/layering"
        -P "${PROJECT_SOURCE_DIR}/cmake/CheckGlmIncludes.cmake")
set_tests_properties(glm_includes_refuses_direct_include PROPERTIES
    LABELS "lint"
    PASS_REGULAR_EXPRESSION "tests/layering/IncludesGlm.cpp includes \"glm/glm.hpp\".*tests/layering/IncludesGlm.cpp includes <glm/vec3.hpp>")

# The file_io_confined check must refuse file I/O outside engine/ith/file_system (ADR-0035 rule
# 10); the fixture is never compiled.
add_test(NAME file_io_confined_refuses_file_io
    COMMAND ${CMAKE_COMMAND} "-DREPOSITORY_DIR=${PROJECT_SOURCE_DIR}" "-DDIRECTORIES=tests/layering"
        -P "${PROJECT_SOURCE_DIR}/cmake/CheckFileIo.cmake")
set_tests_properties(file_io_confined_refuses_file_io PROPERTIES
    LABELS "lint"
    PASS_REGULAR_EXPRESSION "UsesFileIo.cpp includes <filesystem>.*UsesFileIo.cpp includes <fstream>.*UsesFileIo.cpp calls fopen.*UsesFileIo.cpp calls CreateFileW")

# The imgui_confined check must refuse ImGui outside editor_ui (ADR-0029 rule 2): an include of
# imgui.h and of imgui_internal.h, and a CMakeLists.txt that links it. The fixtures are never built.
add_test(NAME imgui_confined_refuses_imgui_outside_editor_ui
    COMMAND ${CMAKE_COMMAND} "-DREPOSITORY_DIR=${PROJECT_SOURCE_DIR}" "-DDIRECTORIES=tests/layering"
        -P "${PROJECT_SOURCE_DIR}/cmake/CheckImGuiConfined.cmake")
set_tests_properties(imgui_confined_refuses_imgui_outside_editor_ui PROPERTIES
    LABELS "lint"
    PASS_REGULAR_EXPRESSION "IncludesImGui.cpp includes imgui.h: only the editor_ui module.*IncludesImGui.cpp includes imgui_internal.h: only engine/ith/editor_ui/DockLayout.cpp.*links_imgui/CMakeLists.txt links ImGui: only")

# The tracy_confined check must refuse Tracy outside ith/foundation/Profile.h and the backends'
# GPU-zone sources (ADR-0007): an include of <tracy/...>, of a Tracy*.hpp header and of a Tracy*.h
# header (TracyC.h), and a CMakeLists.txt that links it. The fixtures are never built.
add_test(NAME tracy_confined_refuses_tracy_outside_profile_h
    COMMAND ${CMAKE_COMMAND} "-DREPOSITORY_DIR=${PROJECT_SOURCE_DIR}" "-DDIRECTORIES=tests/layering"
        -P "${PROJECT_SOURCE_DIR}/cmake/CheckTracyConfined.cmake")
set_tests_properties(tracy_confined_refuses_tracy_outside_profile_h PROPERTIES
    LABELS "lint"
    PASS_REGULAR_EXPRESSION "IncludesTracy.cpp includes TracyVulkan.hpp: profile through.*IncludesTracy.cpp includes TracyC.h: profile through.*IncludesTracy.cpp includes tracy/Tracy.hpp: profile through.*links_tracy/CMakeLists.txt links Tracy: only the foundation module")

ith_add_layering_failure_test(upward_layer
    MODULES "low:core:runtime=high|high:framework:runtime="
    EXPECT "low [(]layer 2[)] depends on high [(]layer 4[)], a higher layer")
ith_add_layering_failure_test(cycle
    MODULES "a:core:runtime=b|b:core:runtime=a"
    EXPECT "a depends on itself through a cycle")
ith_add_layering_failure_test(runtime_on_tools
    MODULES "baker:editor:tools=|net:core:runtime=baker"
    EXPECT "net [(]runtime[)] depends on baker [(]tools[)]")
ith_add_layering_failure_test(runtime_on_client
    MODULES "net:core:runtime=renderer|renderer:core:client="
    EXPECT "net [(]runtime[)] depends on renderer [(]client[)]")
ith_add_layering_failure_test(tools_on_client
    MODULES "renderer:engine:client=|cooker:editor:tools=renderer"
    EXPECT "cooker [(]tools[)] depends on renderer [(]client[)]")
ith_add_layering_failure_test(client_on_editor
    MODULES "outliner:editor:editor=|renderer:engine:client=outliner"
    EXPECT "renderer [(]client[)] depends on outliner [(]editor[)]")
ith_add_layering_failure_test(tools_kind_below_editor_layer
    MODULES "baker:core:tools="
    EXPECT "baker is a tools module in the core layer, but tools modules belong in the editor layer")
ith_add_layering_failure_test(runtime_kind_in_editor_layer
    MODULES "net:editor:runtime="
    EXPECT "net is a runtime module in the editor layer, but runtime modules belong in the layers below editor")
ith_add_layering_failure_test(server_links_client
    MODULES "net:core:runtime=|renderer:engine:client=net|gameplay:framework:client=renderer"
    APPLICATIONS "game_server:server:=gameplay"
    EXPECT "application game_server [(]server[)] links renderer [(]client[)]")
ith_add_layering_failure_test(player_links_tools
    MODULES "net:core:runtime=|baker:editor:tools=net"
    APPLICATIONS "game_player:player:=net,baker"
    EXPECT "application game_player [(]player[)] links baker [(]tools[)]")
ith_add_layering_failure_test(tools_links_editor
    MODULES "outliner:editor:editor="
    APPLICATIONS "cooker:tools:=outliner"
    EXPECT "application cooker [(]tools[)] links outliner [(]editor[)]")
ith_add_layering_failure_test(application_include
    MODULES "net:core:runtime=|renderer:engine:client=net"
    APPLICATIONS "game_server:server:tests/layering/ServerIncludesRenderer.cpp=net"
    EXPECT "tests/layering/ServerIncludesRenderer.cpp includes module 'renderer', which application game_server does not depend on")
ith_add_layering_failure_test(application_unknown_module
    MODULES "net:core:runtime="
    APPLICATIONS "game_server:server:=net,physics"
    EXPECT "application game_server links unknown module 'physics'")
# Only modules declared unavailable on this platform (rhi_d3d12 outside Windows) may be included
# without being declared; any other undeclared module is refused.
ith_add_layering_failure_test(include_of_undeclared_module
    MODULES "net:core:runtime="
    UNAVAILABLE "rhi_d3d12"
    APPLICATIONS "game_server:server:tests/layering/ServerIncludesRenderer.cpp=net"
    EXPECT "tests/layering/ServerIncludesRenderer.cpp includes module 'renderer', which application game_server does not depend on")

# Asset ids and the reflected hard and soft references, through both archives.
ith_add_test_suite(assets
    SOURCES assets/AssetIdTests.cpp
    LIBRARIES ith::assets ith::serialization)

# The asset database (tools only): sidecars, native headers, scan problems, move, rename and
# delete, import and reimport with a test importer, and the derived-data cache. Each test works in
# its own temporary project folder.
if(IRONTHORN_BUILD_TOOLS)
    ith_add_test_suite(asset_database
        SOURCES
            asset_database/AssetTestSupport.h
            asset_database/DerivedDataCacheTests.cpp
            asset_database/ImportTests.cpp
            asset_database/ScanTests.cpp
            asset_database/SidecarTests.cpp
        LIBRARIES ith::asset_database)
endif()

# A libFuzzer target for the sidecar and native header readers, with a short smoke run in CTest
# like the others, in Clang sanitizer builds with the tools on.
if(TARGET ith_asset_database AND IRONTHORN_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"
        AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    add_executable(ith_asset_sidecar_fuzz asset_database/SidecarFuzz.cpp)
    target_link_libraries(ith_asset_sidecar_fuzz PRIVATE ith::asset_database)
    target_compile_options(ith_asset_sidecar_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(ith_asset_sidecar_fuzz PRIVATE -fsanitize=fuzzer)
    set_target_properties(ith_asset_sidecar_fuzz PROPERTIES
        FOLDER "Ironthorn/Tests"
        COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
    set(corpus "${CMAKE_CURRENT_BINARY_DIR}/asset_sidecar_fuzz_corpus")
    file(MAKE_DIRECTORY "${corpus}")
    add_test(NAME asset_sidecar_fuzz
        COMMAND ith_asset_sidecar_fuzz -runs=20000 -max_len=4096 -malloc_limit_mb=512
            -dict=${CMAKE_CURRENT_SOURCE_DIR}/serialization/fuzz_seeds/json.dict
            "${corpus}" "${CMAKE_CURRENT_SOURCE_DIR}/asset_database/fuzz_seeds/sidecar")
    set_tests_properties(asset_sidecar_fuzz PROPERTIES LABELS "fuzz" TIMEOUT 300)
endif()

# The glTF importer (tools only): embedded, external and GLB buffers, accessors, meshes, materials,
# source keys, reimport and the derived-data cache through the asset database, and malformed input.
if(IRONTHORN_BUILD_TOOLS)
    ith_add_test_suite(asset_importers
        SOURCES asset_importers/GltfImporterTests.cpp
        LIBRARIES ith::asset_importers)
endif()

# A libFuzzer target for the glTF importer, with a short smoke run in CTest like the others, in
# Clang sanitizer builds with the tools on.
if(TARGET ith_asset_importers AND IRONTHORN_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"
        AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    add_executable(ith_gltf_import_fuzz asset_importers/GltfFuzz.cpp)
    target_link_libraries(ith_gltf_import_fuzz PRIVATE ith::asset_importers)
    target_compile_options(ith_gltf_import_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(ith_gltf_import_fuzz PRIVATE -fsanitize=fuzzer)
    set_target_properties(ith_gltf_import_fuzz PROPERTIES
        FOLDER "Ironthorn/Tests"
        COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
    set(corpus "${CMAKE_CURRENT_BINARY_DIR}/gltf_import_fuzz_corpus")
    file(MAKE_DIRECTORY "${corpus}")
    add_test(NAME gltf_import_fuzz
        COMMAND ith_gltf_import_fuzz -runs=20000 -max_len=8192 -malloc_limit_mb=1024
            -dict=${CMAKE_CURRENT_SOURCE_DIR}/asset_importers/fuzz_seeds/gltf.dict
            "${corpus}" "${CMAKE_CURRENT_SOURCE_DIR}/asset_importers/fuzz_seeds/gltf")
    set_tests_properties(gltf_import_fuzz PROPERTIES LABELS "fuzz" TIMEOUT 300)
endif()

# Layered configuration (ADR-0035): the merge patch, layers and skipped files, typed sections with
# defaults and warnings, minimal-difference writes, whole files, generations and reload, and the
# engine layer the build copies next to the executables. Test settings types are reflected into
# ith_test_config.
add_library(ith_test_config STATIC)
target_include_directories(ith_test_config PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/config")
target_link_libraries(ith_test_config
    PUBLIC ith::build_options ith::reflection
    PRIVATE ith::runtime_options)
set_target_properties(ith_test_config PROPERTIES
    FOLDER "Ironthorn/Tests"
    COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
ith_reflect_target(ith_test_config
    MODULE test_config
    KIND runtime
    INCLUDE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/config"
    HEADERS config/ConfigTypes.h)
ith_add_test_suite(config
    SOURCES
        config/ConfigTests.cpp
        config/ConfigTypes.h
    LIBRARIES ith::config ith_test_config)
add_dependencies(ith_config_tests ith_engine_config)

# The file system (ADR-0035): path helpers, reads, atomic writes with their fault points, file
# information, listings, directories, remove, move and the known folders, each test in its own
# temporary folder.
ith_add_test_suite(file_system
    SOURCES
        file_system/FileSystemTests.cpp
        file_system/PathTests.cpp
    LIBRARIES ith::file_system Threads::Threads)

# The job system (ADR-0025): deterministic tests without workers, functional and stress tests
# with workers, the wait rules and shutdown. The stress tests are sized for Debug and the
# sanitizer builds.
ith_add_test_suite(jobs
    SOURCES
        jobs/JobHelpersTests.cpp
        jobs/JobStressTests.cpp
        jobs/JobSystemTests.cpp
    LIBRARIES ith::jobs Threads::Threads)

# The deadlock watchdog ends the process, so its test case is skipped in the suite and runs alone
# here, passing when the watchdog's fatal error is printed.
add_test(NAME jobs_watchdog
    COMMAND ith_jobs_tests "--test-case=Watchdog: stops the process*" --no-skip
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
set_tests_properties(jobs_watchdog PROPERTIES
    LABELS "unit"
    TIMEOUT 120
    PASS_REGULAR_EXPRESSION "Job system deadlock")

# Likewise for a foreground pool exhausted by parked continuations (ADR-0026 rule 3).
add_test(NAME jobs_pool_exhausted
    COMMAND ith_jobs_tests "--test-case=JobSystem: stops the process when parked*" --no-skip
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
set_tests_properties(jobs_pool_exhausted PROPERTIES
    LABELS "unit"
    TIMEOUT 120
    PASS_REGULAR_EXPRESSION "foreground job pool exhausted")

# The job system benchmark (docs/development/jobs.md): job overhead, parallelFor speed-up,
# continuation and wake-up latency. Like ecs_benchmark it only prints numbers.
add_executable(ith_jobs_benchmark jobs/JobsBenchmark.cpp)
target_link_libraries(ith_jobs_benchmark PRIVATE ith::jobs Threads::Threads)
set_target_properties(ith_jobs_benchmark PROPERTIES
    FOLDER "Ironthorn/Benchmarks"
    COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
add_test(NAME jobs_benchmark COMMAND ith_jobs_benchmark)
set_tests_properties(jobs_benchmark PROPERTIES LABELS "benchmark" TIMEOUT 600)

# The editor's core (ADR-0037): commands, transactions and undo, selection, files, the keymap,
# automation with the scripts in editor_core/scripts, and the model test, headless. Built wherever
# the editor modules are, sanitizer builds included (ADR-0029 rule 19).
if(TARGET ith_editor_core)
    add_library(ith_test_editor_core_components STATIC)
    target_include_directories(ith_test_editor_core_components PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/editor_core")
    target_link_libraries(ith_test_editor_core_components
        PUBLIC ith::build_options ith::reflection ith::math ith::assets
        PRIVATE ith::runtime_options)
    set_target_properties(ith_test_editor_core_components PROPERTIES
        FOLDER "Ironthorn/Tests"
        COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
    ith_reflect_target(ith_test_editor_core_components
        MODULE test_editor_core
        KIND runtime
        INCLUDE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/editor_core"
        HEADERS editor_core/EditorCoreComponents.h)

    ith_add_test_suite(editor_core
        SOURCES
            editor_core/AutomationTests.cpp
            editor_core/EditCommandTests.cpp
            editor_core/EditorCameraTests.cpp
            editor_core/EditorCoreComponents.h
            editor_core/EditorTestSupport.h
            editor_core/FileTests.cpp
            editor_core/KeymapTests.cpp
            editor_core/LayoutTests.cpp
            editor_core/ModelTests.cpp
            editor_core/TransactionTests.cpp
            editor_core/ViewportTests.cpp
            editor_core/ViewModelTests.cpp
        LIBRARIES ith::editor_core ith_test_editor_core_components)
endif()

# The editor UI (ADR-0029 rule 18), headless: ImGui on our platform backend with injected events
# (routing, text, IME, clipboard, cursor, focus), on our renderer backend on the RHI's null device
# (texture create, update and destroy, upload growth, scissors, recorded commands), the Outliner and
# Details panels driven by mouse and keys, the theme, the fonts, the icons against the pinned
# codepoints.json, the shell and the idle policy. The fonts come from the fonts/ folder the build
# fills next to the executable. Built wherever the editor modules are, sanitizer builds included
# (ADR-0029 rule 19).
if(TARGET ith_editor_ui)
    ith_add_test_suite(editor_ui
        SOURCES
            editor_ui/EditorUiTestSupport.h
            editor_ui/DockLayoutTests.cpp
            editor_ui/EditorUiTests.cpp
            editor_ui/OutputLogTests.cpp
            editor_ui/PanelTests.cpp
            editor_ui/RendererTests.cpp
            editor_ui/ShellTests.cpp
        LIBRARIES ith::editor_ui ith::imgui)
    target_compile_definitions(ith_editor_ui_tests PRIVATE
        "ITH_LUCIDE_CODEPOINTS=\"${font_lucide_SOURCE_DIR}/codepoints.json\"")
    add_dependencies(ith_editor_ui_tests ith_engine_config)
endif()

# The golden screenshot of the empty editor shell (ADR-0029 rule 18) on the software GPU of each
# backend the machine has (WARP, lavapipe), with the UI shader compiled at run time, and analytic
# WARP and lavapipe pixel tests of a user texture drawn by ImGui::Image. The images are binary PPM
# files in tests/editor_ui_gpu/golden, stored with Git LFS; ITH_UPDATE_GOLDEN=1 rewrites them. Needs
# the shader compiler, so not in sanitizer builds.
if(TARGET ith_editor_ui AND ITH_BUILD_SHADER_COMPILER)
    ith_add_test_suite(editor_ui_gpu
        SOURCES
            editor_ui_gpu/ShellGoldenTests.cpp
            editor_ui_gpu/UserTextureTests.cpp
        LIBRARIES ith::editor_ui ith::rhi_select ith::shader_compiler ith::imgui
        LABELS gpu)
    target_compile_definitions(ith_editor_ui_gpu_tests PRIVATE
        "ITH_GOLDEN_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/editor_ui_gpu/golden\"")
    ith_copy_shader_compiler_runtime(ith_editor_ui_gpu_tests)
    add_dependencies(ith_editor_ui_gpu_tests ith_engine_config)
    if(TARGET ith_rhi_d3d12)
        target_compile_definitions(ith_editor_ui_gpu_tests PRIVATE ITH_EDITOR_UI_GPU_HAS_D3D12=1)
        ith_use_d3d12_agility_sdk(ith_editor_ui_gpu_tests)
    endif()
endif()
