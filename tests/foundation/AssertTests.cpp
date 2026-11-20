#include "ith/foundation/Assert.h"

#include "AssertCapture.h"

#include <doctest/doctest.h>

#include <cstring>

namespace {

int g_customHandlerCalls = 0;

#ifdef ITH_ENABLE_ASSERTS
// Returns its argument through an opaque call, so conditions in the tests are not constant
// expressions (MSVC warns about constant conditions, and warnings are errors).
bool runtime(bool value) {
    static volatile bool opaque = true;
    return opaque ? value : !value;
}
#endif

ith::AssertAction countingHandler(const ith::AssertInfo&) {
    ++g_customHandlerCalls;
    return ith::AssertAction::Continue;
}

} // namespace

TEST_SUITE("foundation.assert") {
#ifdef ITH_ENABLE_ASSERTS
    TEST_CASE("a passing assert does not call the handler") {
        ith::test::AssertCapture capture;
        ITH_ASSERT(runtime(true));
        ITH_ASSERT_MSG(runtime(true), "never shown");
        CHECK(capture.count() == 0);
    }

    TEST_CASE("a failing assert reports its expression, message and location") {
        ith::test::AssertCapture capture;
        const bool fourIsThree = runtime(false);
        ITH_ASSERT_MSG(fourIsThree, "value must be four");
        CHECK(capture.count() == 1);
        CHECK(capture.lastExpression() == "fourIsThree");
        CHECK(capture.lastMessage() == "value must be four");
    }

    TEST_CASE("a failing assert without a message reports an empty message") {
        ith::test::AssertCapture capture;
        const bool ready = runtime(false);
        ITH_ASSERT(ready);
        CHECK(capture.count() == 1);
        CHECK(capture.lastExpression() == "ready");
        CHECK(capture.lastMessage().empty());
    }

    TEST_CASE("the handler receives the file and line of the assert") {
        static const char* reportedFile = nullptr;
        static int reportedLine = 0;
        const ith::AssertHandler previous = ith::setAssertHandler([](const ith::AssertInfo& info) {
            reportedFile = info.file;
            reportedLine = info.line;
            return ith::AssertAction::Continue;
        });
        const int expectedLine = __LINE__ + 1;
        ITH_ASSERT(runtime(false));
        ith::setAssertHandler(previous);

        REQUIRE(reportedFile != nullptr);
        CHECK(std::strstr(reportedFile, "AssertTests.cpp") != nullptr);
        CHECK(reportedLine == expectedLine);
    }
#endif

    TEST_CASE("ITH_VERIFY always evaluates its condition") {
        int evaluations = 0;
        ITH_VERIFY(++evaluations == 1);
        CHECK(evaluations == 1);
    }

    TEST_CASE("setAssertHandler returns the previous handler, and nullptr restores the default") {
        const ith::AssertHandler original = ith::setAssertHandler(&countingHandler);
        CHECK(ith::setAssertHandler(original) == &countingHandler);

        const ith::AssertHandler before = ith::setAssertHandler(nullptr);
        // The default handler is reported as nullptr, so it can be restored the same way.
        CHECK(ith::setAssertHandler(before) == nullptr);
    }

#ifdef ITH_ENABLE_ASSERTS
    TEST_CASE("an installed handler is called for every failure") {
        g_customHandlerCalls = 0;
        const ith::AssertHandler previous = ith::setAssertHandler(&countingHandler);
        ITH_ASSERT(runtime(false));
        ITH_ASSERT(runtime(false));
        ith::setAssertHandler(previous);
        CHECK(g_customHandlerCalls == 2);
    }
#endif
}
