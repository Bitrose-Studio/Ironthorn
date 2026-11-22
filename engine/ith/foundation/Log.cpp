#include "ith/foundation/Log.h"

#include "ith/foundation/Mutex.h"
#include "ith/foundation/StringUtils.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

namespace ith {
namespace {

// The registered categories, newest first. Constant-initialized, so it's usable before main.
std::atomic<LogCategory*> g_firstCategory{nullptr};

// True while the calling thread is inside the logger (holding its mutex or calling a sink).
// A record made then goes straight to stderr instead of deadlocking.
thread_local bool g_insideLogger = false;

constexpr std::array<const char*, 6> kLevelNames = {"Trace", "Debug", "Info", "Warning", "Error", "Fatal"};

// Writes one line per record. Error and Fatal records start with the source location in the
// form IDEs and terminals link to. The line is written with one call, so lines written by threads
// that bypass the logger's mutex (see g_insideLogger) don't interleave within a line.
void writeToStderr(void* /*context*/, const LogRecord& record) {
    std::array<char, kLogMessageCapacity + 512> text{};
    const auto levelIndex = static_cast<std::size_t>(record.level);
    const char* level = levelIndex < kLevelNames.size() ? kLevelNames[levelIndex] : "?";
    const char* category = record.category != nullptr ? record.category->name() : "?";
    const int prefix =
        record.level >= LogLevel::Error && record.file != nullptr
            ? std::snprintf(text.data(), text.size(), "%s(%d): [%s] %s: ", record.file, record.line, level, category)
            : std::snprintf(text.data(), text.size(), "[%s] %s: ", level, category);
    if (prefix < 0) {
        return;
    }
    // A prefix cut short by a very long file path still leaves room for the line ending.
    std::size_t size = std::min(static_cast<std::size_t>(prefix), text.size() - 1);
    const std::size_t count = std::min(record.message.size(), text.size() - 1 - size);
    std::memcpy(text.data() + size, record.message.data(), count);
    size += count;
    text[size++] = '\n';
    std::fwrite(text.data(), 1, size, stderr);
    if (record.level >= LogLevel::Warning) {
        std::fflush(stderr);
    }
}

struct Logger {
    Mutex mutex;
    std::array<LogSink, kMaxLogSinks> sinks ITH_GUARDED_BY(mutex) = {LogSink{.write = &writeToStderr}};
    std::size_t sinkCount ITH_GUARDED_BY(mutex) = 1;
};

// The logger is built on first use and never destroyed, so it works before main and during
// static destruction (ADR-0007 rule 8).
Logger& logger() {
    alignas(Logger) static std::array<std::byte, sizeof(Logger)> storage;
    static Logger& instance = *new (storage.data()) Logger();
    return instance;
}

// Sets g_insideLogger for the lifetime of the object.
class InsideLogger {
public:
    InsideLogger() { g_insideLogger = true; }

    ~InsideLogger() { g_insideLogger = false; }

    InsideLogger(const InsideLogger&) = delete;
    InsideLogger& operator=(const InsideLogger&) = delete;
};

} // namespace

namespace detail {

// Gives the registry access to the categories' list links.
struct LogCategoryList {
    static void add(LogCategory& category) {
        LogCategory* first = g_firstCategory.load(std::memory_order_relaxed);
        do {
            category.m_next = first;
        } while (!g_firstCategory.compare_exchange_weak(first, &category, std::memory_order_release,
                                                        std::memory_order_relaxed));
    }

    // Calls `visit` for every registered category, newest first.
    template <typename Visit>
    static void forEach(Visit&& visit) {
        for (LogCategory* category = g_firstCategory.load(std::memory_order_acquire); category != nullptr;
             category = category->m_next) {
            visit(*category);
        }
    }
};

std::size_t finishLogMessage(std::span<char, kLogMessageCapacity> buffer, std::size_t formattedSize) {
    if (formattedSize <= buffer.size()) {
        return formattedSize;
    }
    // Cut before the character that straddles the end, so the message stays valid UTF-8.
    constexpr std::string_view kEllipsis = "...";
    std::size_t end = buffer.size() - kEllipsis.size();
    while (end > 0 && (static_cast<unsigned char>(buffer[end]) & 0xC0u) == 0x80u) {
        --end;
    }
    std::ranges::copy(kEllipsis, buffer.begin() + static_cast<std::ptrdiff_t>(end));
    return end + kEllipsis.size();
}

} // namespace detail

std::string_view logLevelName(LogLevel level) {
    const auto index = static_cast<std::size_t>(level);
    return index < kLevelNames.size() ? std::string_view(kLevelNames[index]) : std::string_view("?");
}

Expected<LogLevel> parseLogLevel(std::string_view name) {
    for (std::size_t index = 0; index < kLevelNames.size(); ++index) {
        if (equalsIgnoreCaseAscii(name, kLevelNames[index])) {
            return static_cast<LogLevel>(index);
        }
    }
    return makeError("log.unknown_level", "Unknown log level '" + std::string(name) +
                                              "'; expected Trace, Debug, Info, Warning, Error or Fatal");
}

void registerLogCategory(LogCategory& category) {
    detail::LogCategoryList::add(category);
}

std::vector<LogCategory*> logCategories() {
    std::vector<LogCategory*> categories;
    detail::LogCategoryList::forEach([&categories](LogCategory& category) { categories.push_back(&category); });
    std::ranges::sort(categories,
                      [](const LogCategory* a, const LogCategory* b) { return std::strcmp(a->name(), b->name()) < 0; });
    return categories;
}

LogCategory* findLogCategory(std::string_view name) {
    LogCategory* found = nullptr;
    detail::LogCategoryList::forEach([&](LogCategory& category) {
        if (found == nullptr && equalsIgnoreCaseAscii(category.name(), name)) {
            found = &category;
        }
    });
    return found;
}

Expected<void> setLogCategoryLevel(std::string_view name, LogLevel level) {
    bool found = false;
    detail::LogCategoryList::forEach([&](LogCategory& category) {
        if (equalsIgnoreCaseAscii(category.name(), name)) {
            category.setLevel(level);
            found = true;
        }
    });
    if (!found) {
        return makeError("log.unknown_category", "No log category is named '" + std::string(name) + "'");
    }
    return {};
}

LogSink stderrLogSink() {
    return LogSink{.write = &writeToStderr};
}

Expected<void> addLogSink(LogSink sink) {
    ITH_ASSERT_MSG(sink.write != nullptr, "a log sink needs a write function");
    ITH_ASSERT_MSG(!g_insideLogger, "addLogSink must not be called from a log sink");
    Logger& state = logger();
    const MutexLock lock(state.mutex);
    const InsideLogger inside;
    const auto installed = std::span(state.sinks).first(state.sinkCount);
    if (std::ranges::find(installed, sink) != installed.end()) {
        return makeError("log.sink_installed", "The log sink is already installed");
    }
    if (state.sinkCount == state.sinks.size()) {
        return makeError("log.sink_limit", "No more log sinks can be installed; remove one first");
    }
    state.sinks[state.sinkCount++] = sink;
    return {};
}

bool removeLogSink(LogSink sink) {
    ITH_ASSERT_MSG(!g_insideLogger, "removeLogSink must not be called from a log sink");
    Logger& state = logger();
    const MutexLock lock(state.mutex);
    const auto installed = std::span(state.sinks).first(state.sinkCount);
    const auto found = std::ranges::find(installed, sink);
    if (found == installed.end()) {
        return false;
    }
    // Keep the order of the others: sinks are called in the order they were installed.
    std::shift_left(found, installed.end(), 1);
    --state.sinkCount;
    return true;
}

void logMessage(LogLevel level, const LogCategory& category, const char* file, int line, std::string_view message) {
    const LogRecord record{.level = level,
                           .category = &category,
                           .message = message,
                           .file = file,
                           .line = line,
                           .time = std::chrono::system_clock::now()};
    if (g_insideLogger) {
        writeToStderr(nullptr, record);
        return;
    }
    Logger& state = logger();
    const MutexLock lock(state.mutex);
    const InsideLogger inside;
    bool wroteToStderr = false;
    for (const LogSink& sink : std::span(state.sinks).first(state.sinkCount)) {
        sink.write(sink.context, record);
        wroteToStderr = wroteToStderr || sink.write == &writeToStderr;
    }
    // A Fatal record ends the process; make sure it is seen even when no sink shows it.
    if (level == LogLevel::Fatal && !wroteToStderr) {
        writeToStderr(nullptr, record);
    }
}

} // namespace ith
