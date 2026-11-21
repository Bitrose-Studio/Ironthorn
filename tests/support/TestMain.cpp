// Entry point shared by every test executable.
//
// The engine is built without exceptions, so doctest runs in its no-exceptions mode
// (DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS): a failed REQUIRE or FAIL is recorded like a
// CHECK and the test case continues. Code after it that relies on it, such as reading the value of
// a failed ith::Expected, then hits an engine assert, which aborts the executable.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <crtdbg.h>
#include <windows.h>

#include <cstdlib>
#endif

namespace {

// Tests run unattended: a failure prints to stderr and ends the process with a non-zero exit
// code instead of waiting on a modal dialog. On Windows this turns off the debug CRT's report
// windows ("abort() has been called", CRT asserts), abort's crash report and the system's crash
// and critical-error boxes.
void disableErrorDialogs() {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
#ifdef _DEBUG
    for (const int type : {_CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
#endif
}

} // namespace

int main(int argc, char** argv) {
    disableErrorDialogs();
    doctest::Context context;
    context.applyCommandLine(argc, argv);
    return context.run();
}
