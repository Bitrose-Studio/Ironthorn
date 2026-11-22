#include "ith/foundation/Log.h"

#include "ith/foundation/LogRingBuffer.h"
#include "ith/foundation/StringUtils.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <thread>
#include <vector>

ITH_LOG_CATEGORY(LogTest);
ITH_LOG_CATEGORY(LogOther);

namespace {

// A loggable engine-style type, to check that std::formatter specializations work.
struct Point {
    int x = 0;
    int y = 0;
};

} // namespace

template <>
struct std::formatter<Point> : std::formatter<int> {
    auto format(const Point& point, std::format_context& context) const {
        return std::format_to(context.out(), "({}, {})", point.x, point.y);
    }
};

namespace {

// Replaces the stderr sink with a ring buffer for the duration of a test, and resets the test
// categories' levels, so tests see exactly their own records.
class CapturedLog {
public:
    CapturedLog() {
        g_logCategoryLogTest.setLevel(ith::LogLevel::Trace);
        g_logCategoryLogOther.setLevel(ith::LogLevel::Info);
        m_hadStderr = ith::removeLogSink(ith::stderrLogSink());
        REQUIRE(ith::addLogSink(m_buffer.sink()));
    }

    ~CapturedLog() {
        ith::removeLogSink(m_buffer.sink());
        if (m_hadStderr) {
            static_cast<void>(ith::addLogSink(ith::stderrLogSink()));
        }
        g_logCategoryLogTest.setLevel(ith::LogLevel::Info);
    }

    CapturedLog(const CapturedLog&) = delete;
    CapturedLog& operator=(const CapturedLog&) = delete;

    std::vector<ith::LogEntry> entries() const {
        std::vector<ith::LogEntry> entries;
        static_cast<void>(m_buffer.read(0, entries));
        return entries;
    }

private:
    ith::LogRingBuffer m_buffer{1024};
    bool m_hadStderr = false;
};

// Ends the process normally when it aborts: CTest fails a test that a signal ended, whatever it
// printed, and the fatal-error tests check what the default assert handler printed before aborting.
void exitWhenAborted() {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
}

// A sink that counts its calls in the int its context points to.
void countingSink(void* context, const ith::LogRecord& /*record*/) {
    ++*static_cast<int*>(context);
}

} // namespace

TEST_SUITE("foundation.log") {
    TEST_CASE("Log: level names round-trip and parse ignoring case") {
        for (const ith::LogLevel level : {ith::LogLevel::Trace, ith::LogLevel::Debug, ith::LogLevel::Info,
                                          ith::LogLevel::Warning, ith::LogLevel::Error, ith::LogLevel::Fatal}) {
            const auto parsed = ith::parseLogLevel(ith::logLevelName(level));
            REQUIRE(parsed);
            CHECK(*parsed == level);
        }
        const auto warning = ith::parseLogLevel("wArNiNg");
        REQUIRE(warning);
        CHECK(*warning == ith::LogLevel::Warning);

        const auto unknown = ith::parseLogLevel("verbose");
        REQUIRE_FALSE(unknown);
        CHECK(unknown.error().code == "log.unknown_level");
    }

    TEST_CASE("Log: records carry level, category, message and location") {
        CapturedLog log;
        const int line = __LINE__ + 1;
        ITH_LOG_WARNING(LogTest, "value {} at {}", 42, Point{1, 2});

        const auto entries = log.entries();
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].level == ith::LogLevel::Warning);
        CHECK(entries[0].category == &g_logCategoryLogTest);
        CHECK(std::string(entries[0].category->name()) == "LogTest");
        CHECK(entries[0].message == "value 42 at (1, 2)");
        CHECK(ith::endsWithIgnoreCaseAscii(entries[0].file, "LogTests.cpp"));
        CHECK(entries[0].line == line);
        CHECK(entries[0].time.time_since_epoch().count() != 0);
    }

    TEST_CASE("Log: shipping builds compile out Trace and Debug, other builds keep every level") {
#ifdef ITH_SHIPPING
        static_assert(ITH_LOG_COMPILED_MIN_LEVEL == static_cast<int>(ith::LogLevel::Info));
#else
        static_assert(ITH_LOG_COMPILED_MIN_LEVEL == static_cast<int>(ith::LogLevel::Trace));
#endif
        CapturedLog log;
        ITH_LOG_TRACE(LogTest, "t");
        ITH_LOG_DEBUG(LogTest, "d");
        ITH_LOG_INFO(LogTest, "i");
        ITH_LOG_WARNING(LogTest, "w");
        ITH_LOG_ERROR(LogTest, "e");

        std::vector<ith::LogLevel> levels;
        for (const ith::LogEntry& entry : log.entries()) {
            levels.push_back(entry.level);
        }
#ifdef ITH_SHIPPING
        CHECK(levels == std::vector{ith::LogLevel::Info, ith::LogLevel::Warning, ith::LogLevel::Error});
#else
        CHECK(levels == std::vector{ith::LogLevel::Trace, ith::LogLevel::Debug, ith::LogLevel::Info,
                                    ith::LogLevel::Warning, ith::LogLevel::Error});
#endif
    }

    TEST_CASE("Log: a statement below its category's level is dropped without evaluating its arguments") {
        CapturedLog log;
        g_logCategoryLogTest.setLevel(ith::LogLevel::Warning);
        int evaluations = 0;
        const auto evaluate = [&evaluations] { return ++evaluations; };

        ITH_LOG_DEBUG(LogTest, "{}", evaluate());
        ITH_LOG_INFO(LogTest, "{}", evaluate());
        CHECK(evaluations == 0);
        ITH_LOG_WARNING(LogTest, "{}", evaluate());
        ITH_LOG_ERROR(LogTest, "{}", evaluate());
        CHECK(evaluations == 2);
        CHECK(log.entries().size() == 2);

        CHECK_FALSE(g_logCategoryLogTest.isEnabled(ith::LogLevel::Info));
        CHECK(g_logCategoryLogTest.isEnabled(ith::LogLevel::Warning));
    }

    TEST_CASE("Log: categories filter independently") {
        CapturedLog log;
        g_logCategoryLogTest.setLevel(ith::LogLevel::Error);
        g_logCategoryLogOther.setLevel(ith::LogLevel::Warning);
        ITH_LOG_WARNING(LogTest, "dropped: LogTest passes Error and above");
        ITH_LOG_WARNING(LogOther, "kept: LogOther passes Warning and above");

        const auto entries = log.entries();
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].category == &g_logCategoryLogOther);
    }

    TEST_CASE("Log: Fatal as a runtime level turns a category off") {
        CapturedLog log;
        g_logCategoryLogTest.setLevel(ith::LogLevel::Fatal);
        ITH_LOG_ERROR(LogTest, "dropped");
        CHECK(log.entries().empty());
    }

    TEST_CASE("Log: categories default to Info") {
        CHECK(ith::g_logCategoryAssert.level() == ith::LogLevel::Info);
        constexpr ith::LogCategory kFresh("Fresh");
        CHECK(kFresh.level() == ith::LogLevel::Info);
    }

    TEST_CASE("Log: categories are registered and found by name") {
        CapturedLog log;
        CHECK(ith::findLogCategory("LogTest") == &g_logCategoryLogTest);
        CHECK(ith::findLogCategory("logtest") == &g_logCategoryLogTest);
        CHECK(ith::findLogCategory("Assert") == &ith::g_logCategoryAssert);
        CHECK(ith::findLogCategory("NoSuchCategory") == nullptr);

        const auto categories = ith::logCategories();
        CHECK(std::ranges::find(categories, &g_logCategoryLogTest) != categories.end());
        CHECK(std::ranges::find(categories, &g_logCategoryLogOther) != categories.end());
        CHECK(std::ranges::is_sorted(categories, [](const ith::LogCategory* a, const ith::LogCategory* b) {
            return std::string_view(a->name()) < std::string_view(b->name());
        }));
    }

    TEST_CASE("Log: setLogCategoryLevel changes a category by name and reports unknown names") {
        CapturedLog log;
        REQUIRE(ith::setLogCategoryLevel("LOGOTHER", ith::LogLevel::Error));
        CHECK(g_logCategoryLogOther.level() == ith::LogLevel::Error);

        const auto unknown = ith::setLogCategoryLevel("NoSuchCategory", ith::LogLevel::Trace);
        REQUIRE_FALSE(unknown);
        CHECK(unknown.error().code == "log.unknown_category");
    }

    TEST_CASE("Log: a message longer than the buffer is truncated and marked") {
        CapturedLog log;
        const std::string longText(ith::kLogMessageCapacity + 100, 'a');
        ITH_LOG_INFO(LogTest, "{}", longText);
        const std::string exact(ith::kLogMessageCapacity, 'b');
        ITH_LOG_INFO(LogTest, "{}", exact);

        const auto entries = log.entries();
        REQUIRE(entries.size() == 2);
        CHECK(entries[0].message.size() == ith::kLogMessageCapacity);
        CHECK(entries[0].message.ends_with("aaa..."));
        CHECK(entries[1].message == exact);
    }

    TEST_CASE("Log: truncation doesn't split a UTF-8 character") {
        CapturedLog log;
        // "é" (two bytes) straddles the point where "..." starts.
        const std::string text = std::string(ith::kLogMessageCapacity - 4, 'a') + "\xC3\xA9" + std::string(100, 'z');
        ITH_LOG_INFO(LogTest, "{}", text);

        const auto entries = log.entries();
        REQUIRE(entries.size() == 1);
        CHECK(ith::isValidUtf8(entries[0].message));
        CHECK(entries[0].message == std::string(ith::kLogMessageCapacity - 4, 'a') + "...");
    }

    TEST_CASE("Log: sinks are added once, removed, and limited in number") {
        CapturedLog log;
        int calls = 0;
        const ith::LogSink counting{.write = &countingSink, .context = &calls};
        REQUIRE(ith::addLogSink(counting));
        const auto again = ith::addLogSink(counting);
        REQUIRE_FALSE(again);
        CHECK(again.error().code == "log.sink_installed");

        ITH_LOG_INFO(LogTest, "one");
        CHECK(calls == 1);
        CHECK(ith::removeLogSink(counting));
        CHECK_FALSE(ith::removeLogSink(counting));
        ITH_LOG_INFO(LogTest, "two");
        CHECK(calls == 1);

        // Fill the table: the logger refuses the sink after the last free slot.
        std::array<int, ith::kMaxLogSinks + 1> contexts{};
        std::size_t added = 0;
        ith::Expected<void> result;
        while (added < contexts.size()) {
            result = ith::addLogSink(ith::LogSink{.write = &countingSink, .context = &contexts[added]});
            if (!result) {
                break;
            }
            ++added;
        }
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "log.sink_limit");
        CHECK(added == ith::kMaxLogSinks - 1); // The capture's ring buffer holds the other slot.
        for (std::size_t index = 0; index < added; ++index) {
            CHECK(ith::removeLogSink(ith::LogSink{.write = &countingSink, .context = &contexts[index]}));
        }
    }

    TEST_CASE("Log: logging from inside a sink goes to stderr instead of the sinks") {
        CapturedLog log;
        static int calls = 0;
        calls = 0;
        const ith::LogSink nested{.write = [](void*, const ith::LogRecord&) {
            ++calls;
            ITH_LOG_INFO(LogOther, "logged from inside a sink (expected in the test output)");
        }};
        REQUIRE(ith::addLogSink(nested));
        ITH_LOG_INFO(LogTest, "outer");
        ith::removeLogSink(nested);

        CHECK(calls == 1);
        const auto entries = log.entries();
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].message == "outer");
    }

    TEST_CASE("Log: the stderr sink can be removed and installed again") {
        const bool installed = ith::removeLogSink(ith::stderrLogSink());
        CHECK(installed);
        CHECK(ith::addLogSink(ith::stderrLogSink()));
    }

    TEST_CASE("Log: threads log concurrently; every sink sees every record in the same order") {
        CapturedLog log;
        constexpr int kThreads = 4;
        constexpr int kRecords = 2000;
        ith::LogRingBuffer first(2 * kThreads * kRecords); // LogOther records are kept too.
        ith::LogRingBuffer second(2 * kThreads * kRecords);
        REQUIRE(ith::addLogSink(first.sink()));
        REQUIRE(ith::addLogSink(second.sink()));
        g_logCategoryLogOther.setLevel(ith::LogLevel::Info);

        std::atomic<bool> stop{false};
        // Readers, level changes and sink changes race the logging threads.
        std::thread reader([&] {
            std::uint64_t next = 0;
            std::vector<ith::LogEntry> entries;
            while (!stop.load()) {
                next = first.read(next, entries);
                entries.clear();
            }
        });
        std::atomic<bool> sinkChangesWorked{true};
        std::thread changer([&] {
            int calls = 0;
            const ith::LogSink counting{.write = &countingSink, .context = &calls};
            while (!stop.load()) {
                g_logCategoryLogOther.setLevel(ith::LogLevel::Error);
                const bool added = ith::addLogSink(counting).hasValue();
                g_logCategoryLogOther.setLevel(ith::LogLevel::Info);
                const bool removed = ith::removeLogSink(counting);
                if (!added || !removed) {
                    sinkChangesWorked.store(false);
                }
            }
        });
        std::vector<std::thread> loggers;
        loggers.reserve(kThreads);
        for (int thread = 0; thread < kThreads; ++thread) {
            loggers.emplace_back([thread] {
                for (int index = 0; index < kRecords; ++index) {
                    ITH_LOG_INFO(LogTest, "{} {}", thread, index);
                    ITH_LOG_INFO(LogOther, "other");
                }
            });
        }
        for (std::thread& logger : loggers) {
            logger.join();
        }
        stop.store(true);
        reader.join();
        changer.join();
        ith::removeLogSink(first.sink());
        ith::removeLogSink(second.sink());
        CHECK(sinkChangesWorked.load());

        std::vector<ith::LogEntry> firstEntries;
        std::vector<ith::LogEntry> secondEntries;
        static_cast<void>(first.read(0, firstEntries));
        static_cast<void>(second.read(0, secondEntries));
        std::erase_if(firstEntries, [](const ith::LogEntry& entry) { return entry.category != &g_logCategoryLogTest; });
        std::erase_if(secondEntries,
                      [](const ith::LogEntry& entry) { return entry.category != &g_logCategoryLogTest; });
        REQUIRE(firstEntries.size() == static_cast<std::size_t>(kThreads * kRecords));
        REQUIRE(secondEntries.size() == firstEntries.size());

        std::array<int, kThreads> nextIndex{};
        bool sameOrder = true;
        bool inThreadOrder = true;
        for (std::size_t index = 0; index < firstEntries.size(); ++index) {
            sameOrder = sameOrder && firstEntries[index].message == secondEntries[index].message;
            // The message is "<thread> <index>".
            const std::string& message = firstEntries[index].message;
            const std::size_t space = message.find(' ');
            REQUIRE(space != std::string::npos);
            int thread = -1;
            int value = -1;
            std::from_chars(message.data(), message.data() + space, thread);
            std::from_chars(message.data() + space + 1, message.data() + message.size(), value);
            REQUIRE(thread >= 0);
            REQUIRE(thread < kThreads);
            inThreadOrder = inThreadOrder && value == nextIndex[static_cast<std::size_t>(thread)];
            ++nextIndex[static_cast<std::size_t>(thread)];
        }
        CHECK(sameOrder);
        CHECK(inThreadOrder);
    }

    // Ends the process through the default assert handler. Skipped in the normal run; the CTest
    // log_fatal_reaches_sinks runs it alone and expects both lines in the output.
    TEST_CASE("Log: a fatal error reaches the sinks and stderr" * doctest::skip()) {
        exitWhenAborted();
        ith::removeLogSink(ith::stderrLogSink());
        REQUIRE(ith::addLogSink(ith::LogSink{.write = [](void*, const ith::LogRecord& record) {
            std::fprintf(stderr, "sink received %.*s: %.*s\n", static_cast<int>(ith::logLevelName(record.level).size()),
                         ith::logLevelName(record.level).data(), static_cast<int>(record.message.size()),
                         record.message.data());
        }}));
        ITH_FATAL("logger fatal test");
    }

    // Ends the process before anything has used the logger. Skipped in the normal run; the CTest
    // log_fatal_before_first_use runs it alone.
    TEST_CASE("Log: a fatal error before the logger was used reaches stderr" * doctest::skip()) {
        exitWhenAborted();
        ITH_FATAL("fatal before the logger was used");
    }
}
