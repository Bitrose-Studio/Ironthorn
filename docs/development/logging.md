# Logging

The logger lives in the `foundation` module (`engine/ith/foundation/Log.h` and `LogRingBuffer.h`).
The design is [ADR-0007](../decisions/0007-logging-and-profiling.md). It uses only the C++
standard library, formats with `std::format`, and needs no initialization: it works before
`main`, during static destruction and while an assert fails.

## Logging from a subsystem

Each subsystem defines one category, at namespace scope in one of its headers, and logs through
the level macros:

```cpp
#include "ith/foundation/Log.h"

namespace ith {
ITH_LOG_CATEGORY(Render); // Defines ith::g_logCategoryRender, named "Render".
}

ITH_LOG_INFO(Render, "Swapchain resized to {}x{}", width, height);
ITH_LOG_WARNING(Render, "Falling back to {}", backendName);
```

| Macro | Level | Meant for |
|---|---|---|
| `ITH_LOG_TRACE` | Trace | Step-by-step detail while debugging one system |
| `ITH_LOG_DEBUG` | Debug | Detail a developer may want, too much for every run |
| `ITH_LOG_INFO` | Info | What the program does: startup, loaded assets, settings |
| `ITH_LOG_WARNING` | Warning | Something went wrong and was handled |
| `ITH_LOG_ERROR` | Error | Something failed and the user will notice |

`Fatal` has no macro: only failed asserts and `ITH_FATAL` produce it, in the `Assert` category.

The macros take the category's token, not an object: `ITH_LOG_INFO(Render, ...)` uses
`g_logCategoryRender`, found by ordinary lookup, so define the category in the namespace of the
code that logs or in an enclosing one. Format strings are checked at compile time. A type becomes
loggable by specializing `std::formatter` for it.

Keep `Log.h` out of headers that many files include: `<format>` is a heavy header. Include it in
`.cpp` files, and in the one header that defines the subsystem's category.

## What a statement costs

- **Compiled out.** Statements below `ITH_LOG_COMPILED_MIN_LEVEL` disappear. It is Trace in Debug
  and development builds and Info in shipping builds (`ITH_SHIPPING`), so Trace and Debug cost
  nothing in what players run. A target or file may define it (0 Trace to 4 Error) before including
  `Log.h`. Stripped statements are still compiled, so they can't rot.
- **Filtered.** Each category has a runtime level, Info by default. A statement below it costs one
  relaxed atomic load and doesn't evaluate its arguments.
- **Logged.** The message is formatted into a 2 KiB buffer on the calling thread's stack, without
  allocating. Longer messages are cut at a UTF-8 character boundary and end in `...`. Report long
  output, such as compiler errors, through its own diagnostics, not one log line.

`ith_log_benchmark` measures these costs (`ctest -L benchmark -R log_benchmark` on a Release build).

## Changing levels at run time

```cpp
g_logCategoryRender.setLevel(ith::LogLevel::Debug);           // in code
ith::setLogCategoryLevel("render", ith::LogLevel::Debug);      // by name, for consoles and options
auto level = ith::parseLogLevel("warning");                    // ith::Expected<LogLevel>
for (ith::LogCategory* category : ith::logCategories()) { ... } // every category, sorted by name
```

Names are matched ignoring ASCII case. Setting a category to `Fatal` turns all of its statements
off. `setLogCategoryLevel` fails with `log.unknown_category` and `parseLogLevel` with
`log.unknown_level`.

## Sinks

A record goes to every installed sink, in the order they were installed, on the calling thread and
under one mutex, so all sinks see records in the same order. A sink is a function and a context
pointer:

```cpp
void writeToMyView(void* context, const ith::LogRecord& record) {
    static_cast<MyView*>(context)->append(record.level, record.category->name(), record.message);
}

if (auto added = ith::addLogSink({.write = &writeToMyView, .context = &view}); !added) {
    // log.sink_installed or log.sink_limit (16 sinks)
}
...
ith::removeLogSink({.write = &writeToMyView, .context = &view}); // before `view` is destroyed
```

The rules for a sink:

- **Be quick.** Every logging thread waits while a sink runs. Copy the record into your own buffer
  and do the slow work elsewhere.
- **Copy what you keep.** The record and its message are valid only during the call.
- **Write Warning and above through.** A Fatal record is followed by the end of the process.
- **Don't add or remove sinks from a sink** (it asserts). Logging from a sink is fine: on that
  thread the record goes straight to stderr instead of deadlocking.
- **Applications install sinks; libraries never do.**

Foundation has two sinks:

- **stderr** (`ith::stderrLogSink()`), installed from the start. It writes one line per record,
  `[Warning] Render: message`, with the source location in front of Error and Fatal records.
  Remove it to keep records off stderr; Fatal records still go to stderr.
- **`ith::LogRingBuffer`**, which keeps the last N records in memory for views that show the log,
  such as the editor's output log and an in-game console. A view reads new records once per frame:

  ```cpp
  ith::LogRingBuffer history(4096);
  ith::addLogSink(history.sink()).value(); // value() asserts that it was installed

  std::vector<ith::LogEntry> entries;
  m_next = history.read(m_next, entries); // records with sequence >= m_next, oldest first
  ```

  A gap in the sequence numbers means records were overwritten before they were read. Each slot
  reuses its message's storage, so once the buffer has wrapped, writing a record allocates only for
  a message longer than any earlier one in that slot. The buffer removes its sink when destroyed.

The log file and debugger output belong in `platform`, the editor's output log in an editor module
(ADR-0007 rule 7); they plug in as sinks.

## Asserts

The default assert handler sends a Fatal record in the `Assert` category to every sink, and to
stderr when no sink writes there, then aborts. Custom assert handlers, such as the tests'
`AssertCapture`, are called instead and don't log.
