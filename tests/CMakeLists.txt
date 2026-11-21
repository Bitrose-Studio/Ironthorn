# Shared test support: doctest's main and helpers such as AssertCapture.
add_library(ith_test_support STATIC
    support/TestMain.cpp
    support/AssertCapture.h
    support/AssertCapture.cpp)
add_library(ith::test_support ALIAS ith_test_support)
target_include_directories(ith_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(ith_test_support PUBLIC ith::build_options ith::foundation doctest::doctest)
# Tests are built without exceptions like the engine. REQUIRE still works: a failed REQUIRE
# ends the test executable instead of only the test case.
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
        foundation/ObjectPoolTests.cpp
        foundation/StopwatchTests.cpp
        foundation/StringUtilsTests.cpp
        foundation/UuidTests.cpp
        foundation/VersionTests.cpp
    LIBRARIES ith::foundation Threads::Threads)

if(IRONTHORN_BUILD_TOOLS)
    ith_add_test_suite(shader_compiler
        SOURCES shader_compiler/ShaderCompilerTests.cpp
        LIBRARIES ith::shader_compiler)
    ith_copy_shader_compiler_runtime(ith_shader_compiler_tests)
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
