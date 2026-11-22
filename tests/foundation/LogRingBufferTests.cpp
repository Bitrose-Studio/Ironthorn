#include "ith/foundation/LogRingBuffer.h"

#include "AssertCapture.h"

#include <doctest/doctest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr ith::LogCategory kCategory("RingTest");

ith::LogRecord record(std::string_view message, ith::LogLevel level = ith::LogLevel::Info) {
    return ith::LogRecord{.level = level, .category = &kCategory, .message = message, .file = "file.cpp", .line = 7};
}

std::vector<ith::LogEntry> readAll(const ith::LogRingBuffer& buffer, std::uint64_t from = 0) {
    std::vector<ith::LogEntry> entries;
    static_cast<void>(buffer.read(from, entries));
    return entries;
}

} // namespace

TEST_SUITE("foundation.log_ring_buffer") {
    TEST_CASE("LogRingBuffer: an empty buffer reads nothing") {
        const ith::LogRingBuffer buffer(4);
        std::vector<ith::LogEntry> entries;
        CHECK(buffer.read(0, entries) == 0);
        CHECK(entries.empty());
        CHECK(buffer.capacity() == 4);
    }

    TEST_CASE("LogRingBuffer: keeps copies of the records with sequence numbers") {
        ith::LogRingBuffer buffer(4);
        {
            const std::string message = "first";
            buffer.write(record(message, ith::LogLevel::Warning));
        } // The entry must not refer to the record's message.
        buffer.write(record("second"));

        const auto entries = readAll(buffer);
        REQUIRE(entries.size() == 2);
        CHECK(entries[0].sequence == 0);
        CHECK(entries[0].message == "first");
        CHECK(entries[0].level == ith::LogLevel::Warning);
        CHECK(entries[0].category == &kCategory);
        CHECK(std::string(entries[0].file) == "file.cpp");
        CHECK(entries[0].line == 7);
        CHECK(entries[1].sequence == 1);
        CHECK(entries[1].message == "second");
    }

    TEST_CASE("LogRingBuffer: a reader gets each record once by passing back the returned sequence") {
        ith::LogRingBuffer buffer(8);
        buffer.write(record("a"));
        buffer.write(record("b"));
        std::vector<ith::LogEntry> entries;
        std::uint64_t next = buffer.read(0, entries);
        CHECK(next == 2);
        CHECK(entries.size() == 2);

        entries.clear();
        next = buffer.read(next, entries);
        CHECK(next == 2);
        CHECK(entries.empty());

        buffer.write(record("c"));
        next = buffer.read(next, entries);
        CHECK(next == 3);
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].message == "c");
    }

    TEST_CASE("LogRingBuffer: wraps around, keeping the newest records") {
        ith::LogRingBuffer buffer(3);
        for (int index = 0; index < 8; ++index) {
            buffer.write(record(std::to_string(index)));
        }
        const auto entries = readAll(buffer);
        REQUIRE(entries.size() == 3);
        CHECK(entries[0].sequence == 5);
        CHECK(entries[0].message == "5");
        CHECK(entries[1].message == "6");
        CHECK(entries[2].message == "7");
    }

    TEST_CASE("LogRingBuffer: a slow reader sees the gap in sequence numbers") {
        ith::LogRingBuffer buffer(2);
        buffer.write(record("0"));
        std::vector<ith::LogEntry> entries;
        const std::uint64_t next = buffer.read(0, entries);
        CHECK(next == 1);
        for (int index = 1; index < 6; ++index) {
            buffer.write(record(std::to_string(index)));
        }
        entries.clear();
        CHECK(buffer.read(next, entries) == 6);
        REQUIRE(entries.size() == 2);
        CHECK(entries[0].sequence == 4); // Records 1 to 3 were overwritten.
        CHECK(entries[1].sequence == 5);
    }

    TEST_CASE("LogRingBuffer: a slot reused for a shorter message holds only that message") {
        ith::LogRingBuffer buffer(1);
        buffer.write(record("a much longer message"));
        buffer.write(record("short"));
        const auto entries = readAll(buffer);
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].message == "short");
    }

    TEST_CASE("LogRingBuffer: clear drops records and keeps counting") {
        ith::LogRingBuffer buffer(4);
        buffer.write(record("a"));
        buffer.write(record("b"));
        buffer.clear();
        CHECK(readAll(buffer).empty());
        buffer.write(record("c"));
        const auto entries = readAll(buffer);
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].sequence == 2);
    }

    TEST_CASE("LogRingBuffer: receives records as an installed sink and uninstalls itself when destroyed") {
        static constinit ith::LogCategory category("RingSinkTest");
        ith::LogSink sink;
        {
            ith::LogRingBuffer buffer(4);
            sink = buffer.sink();
            REQUIRE(ith::addLogSink(sink));
            ith::logMessage(ith::LogLevel::Info, category, "here.cpp", 3, "through the logger");
            const auto entries = readAll(buffer);
            REQUIRE(entries.size() == 1);
            CHECK(entries[0].message == "through the logger");
            CHECK(entries[0].category == &category);
        }
        CHECK_FALSE(ith::removeLogSink(sink));
    }

    TEST_CASE("LogRingBuffer: concurrent writers and a reader") {
        constexpr int kWriters = 4;
        constexpr int kRecords = 5000;
        ith::LogRingBuffer buffer(64);
        std::atomic<bool> done{false};
        std::atomic<bool> ordered{true};
        std::thread reader([&] {
            std::uint64_t next = 0;
            std::vector<ith::LogEntry> entries;
            while (!done.load()) {
                entries.clear();
                const std::uint64_t returned = buffer.read(next, entries);
                for (std::size_t index = 1; index < entries.size(); ++index) {
                    if (entries[index].sequence != entries[index - 1].sequence + 1) {
                        ordered.store(false);
                    }
                }
                if (!entries.empty() && (entries.front().sequence < next || entries.back().sequence + 1 != returned)) {
                    ordered.store(false);
                }
                next = returned;
            }
        });
        std::vector<std::thread> writers;
        writers.reserve(kWriters);
        for (int writer = 0; writer < kWriters; ++writer) {
            writers.emplace_back([&buffer] {
                for (int index = 0; index < kRecords; ++index) {
                    buffer.write(record("message from a writer thread"));
                }
            });
        }
        for (std::thread& writer : writers) {
            writer.join();
        }
        done.store(true);
        reader.join();

        CHECK(ordered.load());
        const auto entries = readAll(buffer);
        REQUIRE(entries.size() == 64);
        CHECK(entries.back().sequence == kWriters * kRecords - 1);
    }

#ifdef ITH_ENABLE_ASSERTS
    TEST_CASE("LogRingBuffer: asserts on zero capacity and still works with one slot") {
        ith::test::AssertCapture capture;
        ith::LogRingBuffer buffer(0);
        CHECK(capture.count() == 1);
        CHECK(buffer.capacity() == 1);
        buffer.write(record("x"));
        CHECK(readAll(buffer).size() == 1);
    }
#endif
}
