#pragma once

// A log sink that keeps the most recent records in memory (ADR-0007), for views that show the
// log while the program runs, such as the editor's output log and an in-game console.
//
//   ith::LogRingBuffer history(4096);
//   ith::addLogSink(history.sink()).value(); // value() asserts that it was installed
//
//   // Each frame, on the thread that draws the view:
//   std::vector<ith::LogEntry> entries;
//   m_nextSequence = history.read(m_nextSequence, entries);
//
// Every record gets a sequence number, counted from 0. The buffer keeps the last `capacity`
// records; older ones are overwritten. A reader passes the sequence number it wants next and
// gets every kept record from there on, so it sees each record once and can tell from a gap in
// the sequence numbers that it read too slowly.
//
// Each slot keeps its message's storage when it's overwritten, so once the buffer has wrapped,
// writing a record allocates only when its message is longer than every earlier message in that
// slot.

#include "ith/foundation/Log.h"
#include "ith/foundation/Mutex.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ith {

// A copy of a LogRecord that owns its message.
struct LogEntry {
    std::uint64_t sequence = 0;
    LogLevel level = LogLevel::Info;
    const LogCategory* category = nullptr; // Never null in an entry that was read.
    std::string message;
    const char* file = nullptr; // Points to the statement's __FILE__, which lives forever.
    int line = 0;
    std::chrono::system_clock::time_point time;
};

class LogRingBuffer {
public:
    // Allocates `capacity` slots. Precondition: capacity > 0.
    explicit LogRingBuffer(std::size_t capacity);

    // Removes this buffer's sink, in case it is still installed.
    ~LogRingBuffer();

    LogRingBuffer(const LogRingBuffer&) = delete;
    LogRingBuffer& operator=(const LogRingBuffer&) = delete;
    LogRingBuffer(LogRingBuffer&&) = delete;
    LogRingBuffer& operator=(LogRingBuffer&&) = delete;

    // The sink to install with addLogSink. It refers to this buffer.
    LogSink sink();

    // The number of records the buffer keeps.
    std::size_t capacity() const { return m_capacity; }

    // Stores a record, overwriting the oldest one when the buffer is full. The sink calls this.
    // Thread-safe.
    void write(const LogRecord& record);

    // Appends to `out`, oldest first, every kept record whose sequence number is at least
    // `fromSequence`, and returns the sequence number of the next record to be written: pass it
    // as `fromSequence` next time. Records overwritten before they were read are skipped.
    // Thread-safe.
    std::uint64_t read(std::uint64_t fromSequence, std::vector<LogEntry>& out) const;

    // Drops the kept records. Sequence numbers continue where they were. Thread-safe.
    void clear();

private:
    const std::size_t m_capacity;
    mutable Mutex m_mutex;
    std::vector<LogEntry> m_slots ITH_GUARDED_BY(m_mutex);     // Record n is in slot n % capacity.
    std::uint64_t m_firstSequence ITH_GUARDED_BY(m_mutex) = 0; // The oldest record kept.
    std::uint64_t m_nextSequence ITH_GUARDED_BY(m_mutex) = 0;  // The next record to be written.
};

} // namespace ith
