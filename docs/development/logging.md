# Logging and profiling

The logger lives in the `foundation` module (`engine/ith/foundation/Log.h` and `LogRingBuffer.h`),
and so do the profiling macros ([Profiling](#profiling)).
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

## Profiling

The profiling macros are in `engine/ith/foundation/Profile.h` (ADR-0007 rules 10 to 13). Engine
and game code profile only through them; the backend is the
[Tracy](https://github.com/wolfpld/tracy) 0.15.0 client.

| Macro | What it records |
|---|---|
| `ITH_PROFILE_SCOPE("name")` | A zone named by a string literal, to the end of the block |
| `ITH_PROFILE_SCOPE_DYNAMIC(name)` | A zone whose name is a run-time string (job names; ADR-0025 rule 18). Costs a `strlen` per call, and a copy of the name while a viewer is connected |
| `ITH_PROFILE_FUNCTION()` | A zone named after the enclosing function |
| `ITH_PROFILE_FRAME()` | The end of a frame; the main loops of the samples and the editor call it once per frame |
| `ITH_PROFILE_PLOT("name", value)` | A point on a plot; integers are plotted as `std::int64_t`, floating-point values as `double` |
| `ITH_PROFILE_THREAD_NAME(name)` | The calling thread's name; the job system names its workers `Foreground N` and `Background N` |

```cpp
#include "ith/foundation/Profile.h"

void Culling::run() {
    ITH_PROFILE_FUNCTION();
    {
        ITH_PROFILE_SCOPE("Frustum test");
        ...
    }
    ITH_PROFILE_PLOT("Visible objects", visibleCount);
}
```

Rules:

- Write every call as a statement ending in `;`, so call sites compile the same with and without
  profiling.
- At most one zone macro per block; open an inner block for a second zone. Zones nest.
- Not in `constexpr` functions: an enabled zone declares static data.
- Without profiling the macros expand to `static_cast<void>(0)` and don't evaluate their
  arguments. A variable used only as a macro argument is unused then: mark it `[[maybe_unused]]`.
- Include `Profile.h` in the `.cpp` files that profile, not in widely included headers: with
  profiling on it includes Tracy's header, which adds about 0.7 to 2 seconds to each translation
  unit. Only `Profile.h` and the GPU-zone sources of the RHI backends (`VulkanProfiler.cpp`,
  `D3D12Profiler.cpp`) may include Tracy, and only `foundation` links it (the `tracy_confined`
  test).

### GPU zones

GPU time is profiled through the RHI's command lists (rule 12), not through a macro:
`rhi::CommandList::beginGpuZone(name)` and `endGpuZone()`. The render graph wraps every pass in a
zone named after the pass, so a frame drawn through it shows each pass's GPU time without more
code. Like the macros, the zones compile to nothing without profiling.

In the viewer, each device's graphics queue is a GPU context of its own, "Vulkan graphics queue"
or "Direct3D 12 graphics queue", and its zones show on that context's timeline, a frame or two
after the GPU ran them: the backend collects them at each `Device::endFrame()`. Like CPU zones,
they are recorded only while a viewer is connected, unless the build collects without one
(`IRONTHORN_PROFILING_ON_DEMAND` off, below).

- **Vulkan** uses `VK_EXT_calibrated_timestamps` with the host clock where the GPU has it, so the
  GPU timeline stays aligned with the CPU's. Without it (lavapipe, for one) the timelines are
  aligned once, when the device is created, and may drift apart over a long session; the backend
  says so on stderr.
- **Direct3D 12** aligns them at every collection.
- **Memory**: per device, a pool or heap of 65,536 timestamp queries (512 KiB), and on Direct3D 12
  a 512 KiB readback buffer outside D3D12MA; 0.5 to 1 MiB of host memory.
- **Lists that never run** (discarded, or in a failed batch) lose their zones' times, and while
  zones are active they turn GPU zones off for that device for good, as stderr says, even in
  on-demand builds once a viewer has connected: recreate the device to get them back
  ([RHI guide](rhi.md#gpu-zones)).

The details, rules and limits are in the [RHI guide](rhi.md#gpu-zones).

### Building with the profiler

The CMake option `IRONTHORN_PROFILING` (off by default) fetches Tracy, builds its client as a
static library and defines `ITH_ENABLE_PROFILING`, which turns the macros on. The profile presets
set it and build the development configuration (RelWithDebInfo):

```powershell
./scripts/dev.ps1 build -Preset windows-profile          # MSVC
./scripts/dev.ps1 build -Preset windows-clang-profile    # clang-cl
./scripts/dev.ps1 test -Preset windows-profile
```

```sh
cmake --preset linux-clang-profile
cmake --build --preset linux-clang-profile
ctest --preset linux-clang-profile
```

With `IRONTHORN_PROFILING`, the option `IRONTHORN_PROFILING_ON_DEMAND` (on by default) builds the
client with `TRACY_ON_DEMAND`. Turned off, every zone, CPU and GPU, is recorded from the start
whether or not a viewer ever connects, and the data stays queued in memory until one does: the
process grows while it runs, so this is for test executables. CI builds the GPU suites that way,
so that the GPU zones write, read and reset their timestamps under the validation layers.

```powershell
./scripts/dev.ps1 configure -Preset windows-profile -DIRONTHORN_PROFILING_ON_DEMAND=OFF
```

Any configuration may be built with the option on, Shipping included, to measure it. A build with
it on is for measuring only and is never distributed: the engine has no packaging step yet, and
the packaging step (roadmap P9) will refuse such builds (ADR-0007 rule 13). CMake refuses the
option together with `IRONTHORN_ENABLE_SANITIZERS` or `IRONTHORN_ENABLE_TSAN`: the client adds
threads and a network listener of its own and, on Linux, replaces `dlclose` for the whole
process, so a sanitizer would check a process that differs from the one that ships.

CI builds and tests `linux-clang-profile` (rule 15), including runs of `window_player` and
`scene_player`, and runs its GPU suites again in a build with `IRONTHORN_PROFILING_ON_DEMAND` off.
The Windows job builds `windows-profile` with that option off and runs its GPU suites on WARP, and
builds `windows-clang-profile`, so MSVC and clang-cl compile the enabled code too. Every other job tests the macros switched off. `clang-tidy` runs
on the `windows-clang` preset, with profiling off, so the enabled branch of `Profile.h` is not
tidied.

### Connecting the viewer

Use the **Tracy 0.15.0 viewer** (network protocol 83). A viewer of another version can't connect
to the client; upgrade both together (`docs/development/third-party.md`). Prebuilt Windows viewers
are on the [release page](https://github.com/wolfpld/tracy/releases/tag/v0.15.0); on Linux, build
the viewer from the same tag.

Start the profiling build, then connect the viewer to `127.0.0.1`. The client is built with six
definitions (five with `IRONTHORN_PROFILING_ON_DEMAND` off), the same in every translation unit:

- `TRACY_ENABLE`: the client is built in.
- `TRACY_ON_DEMAND`: it collects data only while a viewer is connected, so a zone costs one
  check otherwise. The timeline starts when the viewer connects.
- `TRACY_ONLY_LOCALHOST`: every executable that links it (players, the editor, tools and test
  executables) listens on `localhost:8086` while it runs, or on the next free port up to 8105 when
  several run at once. When all 20 ports are taken, the process runs without a listener and can't
  be profiled. Nothing listens on another interface.
- `TRACY_NO_BROADCAST`: it doesn't announce itself on the network, so the viewer's list of
  discovered clients stays empty; enter the address instead.
- `TRACY_NO_CRASH_HANDLER`: Tracy installs no signal handlers or exception filter, so aborts,
  failed asserts and crashes behave as in every other build. Crashes don't appear in the trace.
- `TRACY_TIMER_FALLBACK`: on CPUs and virtual machines without an invariant TSC, Tracy uses a
  portable, lower-resolution timer instead of ending the process at start-up (on Windows with a
  blocking message box).

System tracing and sampling (context switches, CPU usage, call-stack samples) stay compiled in.
They start when the process starts, whether or not a viewer ever connects, but only where the
process is permitted: on Windows an ETW kernel session when it runs elevated, on Linux perf events
when the kernel's `perf_event_paranoid` setting or the process's capabilities allow them.
Otherwise they stay off and the process runs normally. `TRACY_NO_SYS_TRACE=1` in the environment
turns them off.

A trace records the machine's host name and the user name. Share traces with that in mind.

On Linux, Tracy's client replaces `dlclose` with a function that does nothing
(`TracyOverride.cpp`). In a profiling build no shared library in the process is ever unloaded,
whoever loaded it, so a test that unloads and reloads a library passes there without the library
being unloaded.
