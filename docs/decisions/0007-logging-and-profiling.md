# ADR-0007: Logging and profiling

- **Status:** Accepted
- **Date:** 2026-11-20
- **Related:** [ADR-0001](0001-cpp-language-and-safety-rules.md) (no exceptions, no RTTI, asserts),
  [ADR-0004](0004-modules-and-layering.md) (foundation holds logging and profiling hooks; no
  third-party types in public headers), [architecture](../architecture.md)

## Context

Every module needs to report what it is doing and to be measured, from its first line of code.
Changing the logging or profiling API later means touching every call site, so the API is fixed
now. The requirements:

- **No exceptions** (ADR-0001). A library that reports failures by throwing either can't be used
  or turns an expected failure (a log file that can't be opened) into an abort.
- **Foundation layer** (ADR-0004). The logger may use only the C++ standard library. File
  output, debugger output and the editor console live in higher layers and plug in as sinks.
- **No third-party types in public headers** (ADR-0004), unless an ADR allows it.
- **Cheap.** A disabled log statement must not evaluate its arguments. Verbose levels must
  compile out of shipping builds. Profiling must compile to nothing when it is off.
- **Thread-safe**, usable before `main`, during static destruction and while an assert fails.
- **Sinks:** stderr, a log file, the editor's output log and an in-game console.
- **Profiling** of CPU and GPU time (Vulkan and Direct3D 12) on Windows and Linux, with a path
  to consoles.
- **Shipped games** must not open profiler network ports.

Lessons from earlier engine prototypes by the same maintainer:

- A C++ engine wrapped a popular logging library and the `fmt` formatter. It worked, but the
  wrapper's public header exposed `fmt` types, and opening the log file relied on catching the
  library's exception. Neither is allowed here. What worked well was a small engine API with
  categories, an in-memory history and listeners, which fed the editor console and automation.
  Logging state was deliberately never destroyed, so logging stayed usable during shutdown.
- An earlier engine settled on engine-owned logging and profiling macros in the foundation layer,
  with Tracy as the profiling backend behind a build switch that is off by default, in on-demand
  and localhost-only mode, and the required viewer version documented. That design held up.

Versions and sources below were checked on 2026-11-20.

## Options considered

### Logging library

- **spdlog 1.17.0** (MIT, [release](https://github.com/gabime/spdlog/releases/tag/v1.17.0)).
  Mature, many sinks, colour console, async mode. With `SPDLOG_NO_EXCEPTIONS` every internal
  error, including a file sink that fails to open, prints a message and calls `std::abort()`
  ([common.h](https://github.com/gabime/spdlog/blob/v1.17.0/include/spdlog/common.h)). That
  makes an expected failure fatal. It brings `fmt` along (bundled or external), and its bundled
  `fmt/ostream.h` uses `dynamic_cast`. Most of its features (pattern formatter, rotating sinks,
  registry, async pool) would sit behind our own API unused.
- **Quill 13.1.0** (MIT, [release](https://github.com/odygrd/quill/releases/tag/v13.1.0)). Very
  low latency: the caller copies arguments into a queue and a backend thread formats and writes
  them. `QUILL_NO_EXCEPTIONS` aborts on internal errors
  ([QuillError.h](https://github.com/odygrd/quill/blob/v13.1.0/include/quill/core/QuillError.h)).
  The mandatory backend thread, delayed formatting and the extra lifetime rules are more than an
  engine needs at this stage, and the thread must be managed on consoles.
- **Our own minimal logger** in `foundation`, formatting with `std::format` or `fmt`. About
  300 lines: levels, categories, a sink list and a stderr sink. It fits ADR-0001 and ADR-0004
  exactly and keeps third-party code out of the hot path. We maintain it ourselves.

### Formatting

- **printf-style (`vsnprintf`).** No dependency and cheap to compile, but not type-safe.
  GCC and Clang check format strings with `__attribute__((format))`. MSVC only checks them
  under `/analyze`, and engine types (vectors, handles, names) can't be formatted.
- **`std::format` (C++20).** No dependency, type-safe, format strings checked at compile time
  through `std::format_string`, and user types plug in with `std::formatter`. Supported by
  MSVC 19.29+, libstdc++ 13+ and libc++ 17+
  ([compiler support](https://en.cppreference.com/w/cpp/compiler_support/20)), which covers the
  documented minimums (Visual Studio 2022, GCC 13, Clang 18). Without exceptions, a format error
  ends the process: MSVC STL raises through `_invoke_watson` when `_HAS_EXCEPTIONS=0`
  ([yvals.h](https://github.com/microsoft/STL/blob/main/stl/inc/yvals.h)), libstdc++ calls
  `abort` ([manual](https://gcc.gnu.org/onlinedocs/libstdc++/manual/using_exceptions.html)) and
  libc++ calls its verbose abort
  ([format_error.h](https://github.com/llvm/llvm-project/blob/main/libcxx/include/__format/format_error.h)).
  Because format strings are checked at compile time, that only happens for a programmer error,
  which is what an assert would do anyway. `<format>` is a heavy header. Its compile-time cost
  has not been measured for this codebase yet.
- **`fmt` 12.2.1** (MIT, [release](https://github.com/fmtlib/fmt/releases/tag/12.2.1)). The
  origin of `std::format`, a lighter `fmt/base.h`, and faster in some cases. It turns exceptions
  off by itself when the compiler does (`FMT_USE_EXCEPTIONS` is 0 under `-fno-exceptions` or
  `_HAS_EXCEPTIONS=0`), and then reports errors through `fmt::assert_fail`, which aborts and can
  be replaced with `FMT_CUSTOM_ASSERT_FAIL`
  ([base.h](https://github.com/fmtlib/fmt/blob/12.2.1/include/fmt/base.h),
  [format.h](https://github.com/fmtlib/fmt/blob/12.2.1/include/fmt/format.h)). The logging
  macros need `fmt::format_string` in a public header, so it would need an exception to
  ADR-0004, and it adds a dependency.

### Profiler

- **Tracy 0.15.0** (BSD-3-Clause,
  [release](https://github.com/wolfpld/tracy/releases/tag/v0.15.0),
  [manual](https://github.com/wolfpld/tracy/releases/download/v0.15.0/tracy.pdf)). Industry
  standard. CPU zones, frame marks, plots, messages, memory, locks and context switches. GPU
  zones for Vulkan (`TracyVulkan.hpp`, calibrated timestamps) and Direct3D 12 (`TracyD3D12.hpp`).
  The manual measures about 2.25 ns per zone. Without `TRACY_ENABLE` every macro expands to
  nothing. CMake options include `TRACY_ON_DEMAND` (collect only while a viewer is connected),
  `TRACY_ONLY_LOCALHOST` and `TRACY_NO_BROADCAST`. `TRACY_PLATFORM_HEADER` ports it to platforms
  it doesn't support, which is the route to consoles. The client code uses neither exceptions
  nor RTTI. The only `throw` in the client is in the AMD ROCm profiler integration, which we
  don't build. The viewer must use the same network protocol version as the client, and 0.15.0
  has API changes in lock instrumentation, so upgrades need care.
- **Optick 1.4.0** (MIT). CPU and GPU, but its last release was in May 2022
  ([releases](https://github.com/bombomby/optick/releases)).
- **Remotery 1.2.1** (Apache-2.0). Web viewer, but its last release was in July 2022
  ([releases](https://github.com/Celtoys/Remotery/releases)).
- **Platform tools**: PIX event markers ([PixEvents](https://github.com/microsoft/PixEvents),
  MIT), Superluminal and the console vendors' profilers. They are excellent for deep dives on one
  platform, but none covers Windows and Linux with CPU and GPU zones in one timeline. They can be
  added later behind the same macros.
- **Our own profiler.** The roadmap puts an in-editor profiler in P9. Building one now would
  delay P1 for no gain over Tracy.

## Decision

### Logging

1. **Ironthorn has its own minimal logger** in `foundation` (`ith/foundation/Log.h`). It uses
   only the standard library and no logging library.
2. **Formatting uses `std::format`.** Messages are formatted with `std::format_to_n` into a
   fixed stack buffer (2 KiB) on the calling thread, so logging doesn't allocate. Longer messages
   are truncated and marked with `...`. Format strings are checked at compile time. Engine types
   become loggable by specializing `std::formatter`.
3. **Levels:** `Trace`, `Debug`, `Info`, `Warning`, `Error`, and `Fatal`. Only asserts and
   `ITH_FATAL` produce `Fatal`.
4. **Categories.** Each subsystem defines a category object with a name and its own runtime
   level, for example `ITH_LOG_CATEGORY(Render)` in a header. Categories register themselves in a
   list so a console or command-line option can set levels by name.
5. **Macros:** `ITH_LOG_TRACE(category, format, args...)` through `ITH_LOG_ERROR(...)`. A
   statement below the category's runtime level returns before formatting, and its arguments are
   not evaluated.
6. **Compile-time stripping.** `ITH_LOG_COMPILED_MIN_LEVEL` removes statements below it
   entirely. It is `Trace` in Debug and development builds and `Info` in shipping builds, so
   `Trace` and `Debug` statements cost nothing in what players run. The default runtime level is
   `Info`.
7. **Sinks** are a function pointer plus a context pointer. Each receives a record with the level,
   category, message, source file, line and timestamp. Foundation provides only the stderr sink,
   which is built in and works before anything is initialized. The other sinks live where their
   dependencies are:
   - the log file and debugger output in `platform`, which owns the file system and the OS;
   - the editor output log in an `editor` module;
   - the in-game console in the `engine` UI.

   Applications install the sinks they want. Libraries never install sinks.
8. **Synchronous and thread-safe.** A record goes to every sink on the calling thread, under one
   mutex, so all sinks see records in the same order. Sinks must be quick: the file sink buffers
   and flushes on `Warning` and above, and the editor sink copies into its own buffer. A log call
   made from inside a sink on the same thread goes straight to stderr instead of deadlocking.
   Logging state is never destroyed, so logging works during static destruction. There is no
   async mode.
9. **Asserts route into the log.** The default assert handler sends a `Fatal` record (category
   `assert`) to every sink, flushes them, and then breaks or aborts as it does today. This
   replaces its direct `fprintf`. Custom handlers, such as the tests' `AssertCapture`, keep
   working unchanged. An assert raised inside a sink falls back to stderr (rule 8).

### Profiling

10. **Engine-owned profiling macros** in `foundation` (`ith/foundation/Profile.h`):
    `ITH_PROFILE_SCOPE(name)`, `ITH_PROFILE_FUNCTION()`, `ITH_PROFILE_FRAME()`,
    `ITH_PROFILE_PLOT(name, value)` and `ITH_PROFILE_THREAD_NAME(name)`. Engine and game code
    use only these. Memory and lock instrumentation are added with the allocator and threading
    primitives.
11. **Tracy 0.15.0 is the backend.** Without profiling the macros expand to nothing and Tracy
    isn't fetched. With profiling, `Profile.h` includes Tracy's header, because Tracy's zero-cost
    zones need static source-location data at the call site. **This ADR allows that one
    third-party include**, under ADR-0004 rule 5. No Tracy type appears in any engine function
    signature.
12. **GPU zones** are taken inside the Vulkan and Direct3D 12 backends, in private source files,
    through `TracyVulkan.hpp` and `TracyD3D12.hpp`. The RHI exposes an engine-owned scope on its
    command lists. The exact API belongs to the RHI ADR.
13. **Which builds profile.** A CMake option, `IRONTHORN_PROFILING` (off by default), defines
    `ITH_ENABLE_PROFILING` and links the Tracy client as a static library with `TRACY_ENABLE`,
    `TRACY_ON_DEMAND`, `TRACY_ONLY_LOCALHOST` and `TRACY_NO_BROADCAST`. Profile presets build
    the development configuration with it on. Any configuration may turn it on, including
    shipping for measuring, but packaging (P9) refuses to package a build that has it on.
14. **Tracy is pinned** by URL and SHA-256 like every dependency, and the matching viewer
    version is documented in `docs/development/third-party.md`.
15. **CI builds one Linux configuration with profiling on**, so the enabled macros keep
    compiling. The job is added together with the first profiling call in engine code. All
    other jobs test the disabled path.

## Consequences

- No logging dependency. Logging follows ADR-0001 and ADR-0004 without exceptions to either
  rule, and an expected failure such as an unwritable log file is reported by the platform sink
  with `ith::Expected` instead of aborting.
- One logging pipeline feeds stderr, the log file, the editor output log and the in-game
  console, and assert failures appear in all of them.
- Every translation unit that logs pays for `<format>`. Measure it when the logger lands, and
  keep `Log.h` out of widely included headers.
- A synchronous mutex means a slow sink can stall the threads that log. This is acceptable at the
  volume a game logs with `Trace` and `Debug` off.
- Messages longer than 2 KiB are truncated. Long tool output, such as shader compiler errors,
  should be reported through its own diagnostics, not one log line.
- Profiling costs nothing unless it's built in. Profiling builds listen only on localhost and
  collect only while a viewer is connected.
- Follow-up work: the logger and profiler macros with tests; the platform file and debugger
  sinks; the RHI GPU-zone API; a sink that forwards warnings and errors to the profiler timeline;
  profile presets and the CI job.
- **Revisit when:**
  - a target toolchain (a console SDK in particular) lacks a usable `<format>`, or its compile
    time proves too high. Then the formatter moves to `fmt` behind the same macros, with an ADR
    allowing its type in `Log.h`.
  - profiles show logging cost on hot threads. Then add an asynchronous sink, not an async
    logger.
  - engine modules become shared libraries. Tracy then needs its shared client.
  - Tracy stops being maintained, or a console needs its vendor profiler. Add a backend behind
    the same macros.
