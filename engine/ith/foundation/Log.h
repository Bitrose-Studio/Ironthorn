#pragma once

// Logging (ADR-0007): categories with runtime levels, compile-time stripping, std::format
// messages and a list of sinks.
//
//   // In a header of the subsystem, at namespace scope:
//   ITH_LOG_CATEGORY(Render);
//
//   // Anywhere the category is visible:
//   ITH_LOG_INFO(Render, "Swapchain resized to {}x{}", width, height);
//   ITH_LOG_WARNING(Render, "Falling back to {}", backendName);
//
// A statement below its category's runtime level returns before formatting and doesn't evaluate
// its arguments. A statement below ITH_LOG_COMPILED_MIN_LEVEL is compiled out but still
// type-checked, format string included. Messages are formatted with std::format into a 2 KiB
// buffer on the calling thread, so logging doesn't allocate; longer messages are truncated and
// end in "...". Engine types become loggable by specializing std::formatter.
//
// Each record goes to every installed sink on the calling thread, under one mutex, so every sink
// sees the records in the same order. The stderr sink is installed from the start, so logging
// works before main, during static destruction and while an assert fails: nothing needs to be
// initialized, and the logger's state is never destroyed. Applications install the other sinks
// (a log file, the editor's output log, an in-game console); libraries never install sinks.
//
// Keep this header out of widely included headers: it includes <format>.

#include "ith/foundation/Expected.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// The lowest level whose statements are compiled in, as a number (0 Trace, 1 Debug, 2 Info,
// 3 Warning, 4 Error). Trace in Debug and development builds, Info in shipping builds
// (ITH_SHIPPING). A translation unit or target may define it before including this header.
#ifndef ITH_LOG_COMPILED_MIN_LEVEL
#ifdef ITH_SHIPPING
#define ITH_LOG_COMPILED_MIN_LEVEL 2
#else
#define ITH_LOG_COMPILED_MIN_LEVEL 0
#endif
#endif

namespace ith {

namespace detail {
struct LogCategoryList;
} // namespace detail

// How important a record is. Fatal is produced only by failed asserts and ITH_FATAL; there is
// no logging macro for it.
enum class LogLevel : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Fatal,
};

// The level's name ("Trace" ... "Fatal").
std::string_view logLevelName(LogLevel level);

// Parses a level name, ignoring ASCII case ("warning", "Warning"). Fails with
// "log.unknown_level" for anything else.
Expected<LogLevel> parseLogLevel(std::string_view name);

// A named source of log records with its own runtime level, one per subsystem. Define categories
// with ITH_LOG_CATEGORY, which makes them constant-initialized (usable before any constructor
// runs) and registers them, so they can be found and changed by name. A category is never
// unregistered and must live until the process ends.
class LogCategory {
public:
    // Precondition: `name` is a string literal or otherwise lives until the process ends.
    constexpr explicit LogCategory(const char* name, LogLevel level = LogLevel::Info) : m_name(name), m_level(level) {}

    ~LogCategory() = default; // Trivial, so the category stays usable during static destruction.
    LogCategory(const LogCategory&) = delete;
    LogCategory& operator=(const LogCategory&) = delete;

    const char* name() const { return m_name; }

    // The lowest level this category passes on. Thread-safe.
    LogLevel level() const { return m_level.load(std::memory_order_relaxed); }

    // Changes the runtime level. Thread-safe; logging threads see the change soon, not at once.
    // LogLevel::Fatal turns every logging statement of the category off.
    void setLevel(LogLevel level) { m_level.store(level, std::memory_order_relaxed); }

    // True when a statement of `level` would be passed on. Thread-safe.
    bool isEnabled(LogLevel level) const { return level >= this->level(); }

private:
    friend struct detail::LogCategoryList;

    const char* m_name;
    std::atomic<LogLevel> m_level;
    LogCategory* m_next = nullptr; // Set once, before the category becomes visible in the list.
};

// Adds a category to the list of registered categories. ITH_LOG_CATEGORY calls it; register a
// category only once. Lock-free and thread-safe; safe before main.
void registerLogCategory(LogCategory& category);

// Every registered category, sorted by name. Thread-safe.
std::vector<LogCategory*> logCategories();

// The registered category with this name, ignoring ASCII case, or nullptr. Thread-safe.
LogCategory* findLogCategory(std::string_view name);

// Sets the runtime level of every registered category with this name (ASCII case ignored), for
// consoles and command-line options. Fails with "log.unknown_category" when there is none.
// Thread-safe.
Expected<void> setLogCategoryLevel(std::string_view name, LogLevel level);

// One log record, as a sink receives it. The record and the strings it points to are valid only
// during the sink call; a sink that keeps a record copies the message.
struct LogRecord {
    LogLevel level = LogLevel::Info;
    const LogCategory* category = nullptr; // Never null; categories live until the process ends.
    std::string_view message;              // UTF-8, at most kLogMessageCapacity bytes, without a trailing newline.
    const char* file = nullptr;            // Source file of the statement (__FILE__).
    int line = 0;
    std::chrono::system_clock::time_point time; // When the record was made.
};

// The size of the formatting buffer: longer messages are truncated.
inline constexpr std::size_t kLogMessageCapacity = 2048;

// The most sinks that can be installed at once.
inline constexpr std::size_t kMaxLogSinks = 16;

// A destination for log records: a function and the context it is called with.
//
// Sinks are called on the logging thread while the logger's mutex is held, so they must be quick
// and must not wait for other threads that log. A sink that buffers writes records of
// LogLevel::Warning and above through before returning, because a Fatal record is followed by
// the end of the process. Logging from inside a sink is allowed: on that thread the record goes
// straight to stderr instead of to the sinks.
struct LogSink {
    void (*write)(void* context, const LogRecord& record) = nullptr;
    void* context = nullptr;

    friend bool operator==(const LogSink&, const LogSink&) = default;
};

// The built-in sink, which writes one line per record to stderr and is installed from the
// start. Remove it with removeLogSink(stderrLogSink()) to keep records off stderr; Fatal records
// are written to stderr even then.
LogSink stderrLogSink();

// Installs a sink; from now on it receives every record. Fails with "log.sink_limit" when
// kMaxLogSinks are installed and with "log.sink_installed" when this sink already is.
// Precondition: `sink.write` is not null. Thread-safe.
Expected<void> addLogSink(LogSink sink);

// Uninstalls a sink. Returns false when it wasn't installed. After it returns, the sink is not
// called again, so its context may be destroyed. Thread-safe; must not be called from a sink.
bool removeLogSink(LogSink sink);

// Sends a finished message to every sink. The logging macros and the assert handler call it;
// other code uses the macros. Thread-safe.
void logMessage(LogLevel level, const LogCategory& category, const char* file, int line, std::string_view message);

namespace detail {

// True when statements of `level` are compiled in. A function, so that comparing against a
// minimum of 0 doesn't warn that the comparison is always true.
constexpr bool isLogLevelCompiled(LogLevel level, int compiledMinLevel) {
    return static_cast<int>(level) >= compiledMinLevel;
}

// Registers a category when constructed; the second half of ITH_LOG_CATEGORY.
struct LogCategoryRegistration {
    explicit LogCategoryRegistration(LogCategory& category) { registerLogCategory(category); }
};

// Turns a format_to_n result into the message length, marking a truncated message with "...".
std::size_t finishLogMessage(std::span<char, kLogMessageCapacity> buffer, std::size_t formattedSize);

template <typename... Args>
void logFormatted(LogLevel level, const LogCategory& category, const char* file, int line,
                  std::format_string<Args...> format, Args&&... args) {
    // Not zeroed: format_to_n writes what is read, and logging must stay cheap.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    std::array<char, kLogMessageCapacity> buffer;
    const auto result = std::format_to_n(buffer.data(), static_cast<std::ptrdiff_t>(buffer.size()), format,
                                         std::forward<Args>(args)...);
    const std::size_t size = finishLogMessage(buffer, static_cast<std::size_t>(result.size));
    logMessage(level, category, file, line, std::string_view(buffer.data(), size));
}

} // namespace detail

} // namespace ith

// Defines the log category `Name` (named "Name") at namespace scope, typically in a header of the
// subsystem. The logging macros take the same token: ITH_LOG_INFO(Name, ...). The category object
// is g_logCategory<Name>; it's found by ordinary lookup from where the macros are used, so define
// it in the namespace of its users or an enclosing one.
#define ITH_LOG_CATEGORY(Name)                                                                                         \
    inline constinit ::ith::LogCategory g_logCategory##Name{#Name};                                                    \
    inline const ::ith::detail::LogCategoryRegistration g_logCategory##Name##Registration {                            \
        g_logCategory##Name                                                                                            \
    }

namespace ith {
// The category of the Fatal records of failed asserts and ITH_FATAL (ADR-0007 rule 9).
ITH_LOG_CATEGORY(Assert);
} // namespace ith

// ITH_LOG(Name, Level, format, args...): logs at ::ith::LogLevel::Level. Prefer the per-level
// macros below.
#define ITH_LOG(Name, Level, ...)                                                                                      \
    do {                                                                                                               \
        if constexpr (::ith::detail::isLogLevelCompiled(::ith::LogLevel::Level, ITH_LOG_COMPILED_MIN_LEVEL)) {         \
            if (g_logCategory##Name.isEnabled(::ith::LogLevel::Level)) {                                               \
                ::ith::detail::logFormatted(::ith::LogLevel::Level, g_logCategory##Name, __FILE__, __LINE__,           \
                                            __VA_ARGS__);                                                              \
            }                                                                                                          \
        }                                                                                                              \
    } while (false)

#define ITH_LOG_TRACE(Name, ...) ITH_LOG(Name, Trace, __VA_ARGS__)
#define ITH_LOG_DEBUG(Name, ...) ITH_LOG(Name, Debug, __VA_ARGS__)
#define ITH_LOG_INFO(Name, ...) ITH_LOG(Name, Info, __VA_ARGS__)
#define ITH_LOG_WARNING(Name, ...) ITH_LOG(Name, Warning, __VA_ARGS__)
#define ITH_LOG_ERROR(Name, ...) ITH_LOG(Name, Error, __VA_ARGS__)
