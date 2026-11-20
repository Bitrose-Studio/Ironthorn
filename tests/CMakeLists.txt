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

ith_add_test_suite(foundation
    SOURCES
        foundation/AssertTests.cpp
        foundation/ExpectedTests.cpp
        foundation/VersionTests.cpp
    LIBRARIES ith::foundation)
