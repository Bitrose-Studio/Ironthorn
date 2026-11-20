// Entry point shared by every test executable.
//
// The engine is built without exceptions, so doctest runs in its no-exceptions mode
// (DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS): a failed REQUIRE ends the test
// executable, while CHECK records the failure and continues.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

int main(int argc, char** argv) {
    doctest::Context context;
    context.applyCommandLine(argc, argv);
    return context.run();
}
