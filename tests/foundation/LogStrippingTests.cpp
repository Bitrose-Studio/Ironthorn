// Compile-time stripping (ADR-0007 rule 6), tested by compiling this file as a shipping-like
// build that keeps only Warning and above. Defining the level before the first include is what a
// target or translation unit does to override the build's default.
#define ITH_LOG_COMPILED_MIN_LEVEL 3

#include "ith/foundation/Log.h"
#include "ith/foundation/LogRingBuffer.h"

#include <doctest/doctest.h>

ITH_LOG_CATEGORY(StripTest);

TEST_SUITE("foundation.log_stripping") {
    TEST_CASE("Log: statements below the compiled minimum are stripped, whatever the runtime level") {
        g_logCategoryStripTest.setLevel(ith::LogLevel::Trace);
        ith::LogRingBuffer buffer(16);
        const bool hadStderr = ith::removeLogSink(ith::stderrLogSink());
        REQUIRE(ith::addLogSink(buffer.sink()));

        int evaluations = 0;
        const auto evaluate = [&evaluations] { return ++evaluations; };
        ITH_LOG_TRACE(StripTest, "{}", evaluate());
        ITH_LOG_DEBUG(StripTest, "{}", evaluate());
        ITH_LOG_INFO(StripTest, "{}", evaluate());
        CHECK(evaluations == 0);
        ITH_LOG_WARNING(StripTest, "{}", evaluate());
        ITH_LOG_ERROR(StripTest, "{}", evaluate());
        CHECK(evaluations == 2);

        ith::removeLogSink(buffer.sink());
        if (hadStderr) {
            static_cast<void>(ith::addLogSink(ith::stderrLogSink()));
        }
        std::vector<ith::LogEntry> entries;
        static_cast<void>(buffer.read(0, entries));
        REQUIRE(entries.size() == 2);
        CHECK(entries[0].level == ith::LogLevel::Warning);
        CHECK(entries[1].level == ith::LogLevel::Error);
    }
}
