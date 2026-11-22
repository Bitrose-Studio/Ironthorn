#include "ith/foundation/LogRingBuffer.h"

#include "ith/foundation/Assert.h"

#include <algorithm>

namespace ith {
namespace {

void writeToRingBuffer(void* context, const LogRecord& record) {
    static_cast<LogRingBuffer*>(context)->write(record);
}

} // namespace

LogRingBuffer::LogRingBuffer(std::size_t capacity) : m_capacity(std::max<std::size_t>(capacity, 1)) {
    ITH_ASSERT_MSG(capacity > 0, "a LogRingBuffer needs at least one slot");
    const MutexLock lock(m_mutex);
    m_slots.resize(m_capacity);
}

LogRingBuffer::~LogRingBuffer() {
    removeLogSink(sink());
}

LogSink LogRingBuffer::sink() {
    return LogSink{.write = &writeToRingBuffer, .context = this};
}

void LogRingBuffer::write(const LogRecord& record) {
    const MutexLock lock(m_mutex);
    LogEntry& entry = m_slots[m_nextSequence % m_capacity];
    entry.sequence = m_nextSequence;
    entry.level = record.level;
    entry.category = record.category;
    entry.message.assign(record.message); // Reuses the slot's storage when it is large enough.
    entry.file = record.file;
    entry.line = record.line;
    entry.time = record.time;
    ++m_nextSequence;
    if (m_nextSequence - m_firstSequence > m_capacity) {
        m_firstSequence = m_nextSequence - m_capacity; // The oldest record was overwritten.
    }
}

std::uint64_t LogRingBuffer::read(std::uint64_t fromSequence, std::vector<LogEntry>& out) const {
    const MutexLock lock(m_mutex);
    for (std::uint64_t sequence = std::max(fromSequence, m_firstSequence); sequence < m_nextSequence; ++sequence) {
        out.push_back(m_slots[sequence % m_capacity]);
    }
    return m_nextSequence;
}

void LogRingBuffer::clear() {
    const MutexLock lock(m_mutex);
    m_firstSequence = m_nextSequence;
}

} // namespace ith
