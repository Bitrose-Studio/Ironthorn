# Jobs

The `jobs` module (`engine/ith/jobs`) runs the engine's parallel work: frame work on foreground
workers, long work on background workers, and results back on the main thread. The design is
[ADR-0025](../decisions/0025-job-system.md), with the corrections of
[ADR-0026](../decisions/0026-job-system-rules-corrected.md). It is a `runtime` module in the `platform` layer
and uses only the C++ standard library.

## The job system

The application creates one `ith::JobSystem` on the main thread at startup and passes it to the
subsystems that use it. There is no global instance.

```cpp
#include "ith/jobs/JobSystem.h"

ith::JobSystem jobs;                                    // default worker counts
ith::JobSystem testJobs({.foregroundWorkers = 0,        // tests: no workers, everything runs
                         .backgroundWorkers = 0});      // inside wait(), in submission order
```

| Setting | Default | Meaning |
|---|---|---|
| `foregroundWorkers` | hardware threads - 1 | Frame workers, normal OS priority. The main thread is the extra one: it runs foreground jobs while it waits. |
| `backgroundWorkers` | hardware threads / 4, 1 to 4 | Workers for long jobs, below-normal OS priority. |
| `foregroundJobCapacity` | 4,096 | Records for queued and parked foreground jobs. Fixed: frame work never allocates. |

Workers are named `Foreground N` and `Background N` for debuggers and profilers.
Background workers run below normal priority, so on a machine saturated by other
normal-priority work they can be starved until the load drops; tests and tools that wait on
background work slow down accordingly.

### Lanes

| Lane | For | Rules |
|---|---|---|
| `JobLane::Foreground` | Frame work: culling, animation, command recording, ECS systems | Short (guideline under 1 ms). Never blocks on files, sockets or OS waits other than job waits. |
| `JobLane::Background` | Import, cooking, shader compilation, DDC, streaming, editor operations | May block on files. Foreground workers never run these. |
| `JobLane::MainThread` | Results that touch main-thread state (asset database, editor UI, windows) | Run by `runMainThreadJobs()`, once per frame. |

### Jobs and counters

A job is a name (a string literal) and a callable that takes no arguments and returns nothing.
Its captures are stored inline, at most 48 bytes (`kJobCaptureSize`); a larger capture doesn't
compile, so capture a pointer to your own data instead.

```cpp
ith::JobCounter counter;                                // owned by the caller
jobs.submit(ith::JobLane::Foreground, "Animate", [&] { animate(); }, &counter);
jobs.submit(ith::JobLane::Foreground, "Cull", [&] { cull(); }, &counter);
jobs.wait(counter);                                     // the main thread helps meanwhile
```

- A `JobCounter` counts the unfinished jobs submitted with it. It can't be copied or moved, and
  destroying it while jobs are pending asserts. Once `wait` returns or `isDone()` is true, the
  job system no longer touches it.
- `submitAfter(counter, lane, name, job)` queues a job once a counter is done, without blocking
  a thread. Chains are built this way:

  ```cpp
  jobs.submit(ith::JobLane::Background, "Import", [&] { import(); }, &imported);
  jobs.submitAfter(imported, ith::JobLane::Background, "Write DDC", [&] { write(); }, &written);
  jobs.submitAfter(written, ith::JobLane::MainThread, "Update database", [&] { update(); });
  ```

- `postToMainThread(name, job)` is `submit` on `JobLane::MainThread`.
  `runMainThreadJobs()` runs the jobs queued before the call; jobs they post run next time.

### parallelFor

```cpp
jobs.parallelFor("Cull", objects.size(), 256, [&](std::size_t begin, std::size_t end) {
    for (std::size_t index = begin; index < end; ++index) {
        visible[index] = isVisible(objects[index]);
    }
});
```

The ranges are `[0, 256)`, `[256, 512)` and so on, whatever the worker count; workers and the
calling thread claim them from a shared cursor. The order ranges run in is not defined, so a
result that must be deterministic (ADR-0025 rule 17) is built from per-range outputs merged in
range order, never from a shared accumulator whose result depends on timing.

### Waiting

These are ADR-0025's rules as corrected by
[ADR-0026](../decisions/0026-job-system-rules-corrected.md) ("job system rules corrected").
While its counter isn't done, `wait` runs queued jobs:

- its own lane: a worker runs jobs of its lane and the main thread runs foreground jobs, up to
  8 nested waits per thread (`kMaxHelpingWaitDepth`);
- foreground and background lanes configured without workers, by any waiting thread (external
  threads included), at any depth and whatever lanes its counter waits for, because nobody else
  would run them (this is what makes zero-worker tests run everything inline);
- main-thread jobs the counter waits for, on the main thread outside any job.

The counter's lanes are read again on every pass, so a job that adds work on another lane to
the counter being waited on is seen. With nothing to run, the waiter sleeps on one wait epoch
owned by the `JobSystem` (`std::atomic::wait`) until the counter is done or a job it may run is
queued.

Rules, asserted in Debug and development builds:

- no `wait` while the thread holds an `ith::Mutex` (`ith::heldMutexCount()`);
- a foreground job never waits on background jobs; poll the counter or use a continuation;
- no job waits on main-thread jobs;
- a continuation is never counted by the counter it follows (`submitAfter(c, ..., &c)`); if
  the assert continues, the job is queued at once.

The main thread may wait on background jobs only outside the frame loop: at shutdown, or in
command-line tools such as the cooker.

**Prefer continuations to waits inside jobs.** A waiting thread runs other jobs, and those may
wait too, so nested waits stack up. When every foreground worker **and** the main thread sleep in
waits past the helping depth while foreground jobs stay queued for about a second, the watchdog
ends the process with `ITH_FATAL("Job system deadlock: ...")`. A main thread that is busy, not
waiting, is no deadlock: it will run the queued jobs when it next waits. The watchdog only sees
threads asleep in `wait`: a main thread blocked anywhere else (in `std::thread::join` or an OS
wait) while the foreground lane deadlocks makes the process hang without the fatal error. The background lane has
no watchdog: its jobs may legitimately run for seconds, so a stuck background lane shows up as
an operation that never finishes, not as a fatal error.

### The foreground pool limit

The 4,096 foreground records (`foregroundJobCapacity`) are a limit to design within. Queued jobs
and continuations parked on a counter hold a record; running jobs don't. When the pool is full,
a submitting thread runs queued foreground jobs to free records. If nothing is runnable for
about a second, because parked continuations hold the records, it stops the process with
`ITH_FATAL("Job system: foreground job pool exhausted by parked continuations ...")`. Frame work
that parks thousands of continuations at once is restructured, or the application raises the
setting.

### Cancellation, progress and errors

```cpp
ith::CancellationToken cancel;       // the operation owns these; jobs point at them
ith::Progress progress;
ith::FirstError<> errors;

progress.addWork(static_cast<std::uint32_t>(files.size()));
for (std::size_t index = 0; index < files.size(); ++index) {
    jobs.submit(ith::JobLane::Background, "Import", [&, index] {
        if (cancel.isCancelled() || errors.failed()) {
            return;
        }
        errors.check(index, importFile(files[index]));
        progress.complete();
    }, &counter);
}
```

- **Cancellation is cooperative.** Queued jobs still run; they see the flag and return.
- **Progress** is one atomic done/total pair; the editor reads `snapshot()` once per frame.
- **Jobs return nothing.** Results, `ith::Expected` errors included, go to storage the caller
  owns. `FirstError` keeps the error with the lowest order (index), so the reported error
  doesn't depend on timing.

### Shutdown

Destroying the `JobSystem` (on the main thread, outside any job) sets `shutdownRequested()`,
which long background jobs should check, then runs or waits for every queued, running and
parked job of every lane, main-thread jobs included, and joins the workers. Jobs may submit
while it drains; nothing is dropped.

## How it works

- Foreground job records come from a fixed pool, background and main-thread records from a
  growable one. A job moves its callable to the running thread's stack when it starts, so
  records are held only by queued and parked jobs. When the foreground pool is full, the
  submitting thread runs queued foreground jobs until a record frees.
- Each worker has a FIFO queue behind an `ith::Mutex`. A worker uses its own queue first and
  takes from the others when it runs dry; other threads push round-robin. Idle workers spin
  briefly, then sleep on the lane's condition variable.
- Waiting threads sleep with `std::atomic::wait` on the system's wait epoch, which moves when a
  counter reaches zero and when a job is queued that no idle worker will pick up. The last job of a counter takes the counter's
  mutex for the final decrement, so a thread that sees the counter done under that mutex knows
  the job system is finished with it.
- Only standard atomics, mutexes and `std::atomic::wait`; no standalone fences, which
  ThreadSanitizer doesn't model. The stress tests run under TSan and ASan in CI.

## Tests and benchmark

- `tests/jobs/`: deterministic tests without workers, functional tests, the wait rules, shutdown,
  the watchdog's rule, and stress tests (random job trees with nested waits and continuations,
  a tiny pool, counter lifetimes, nested `parallelFor`, cancellation, shutdown with work in
  flight), and regression tests for the ADR-0026 corrections (lanes gained while waiting,
  zero-worker lanes helped by any waiter, a busy main thread that isn't a deadlock). The CTests
  `jobs_watchdog` and `jobs_pool_exhausted` deadlock a job system on purpose and pass when the
  watchdog, or the pool-exhaustion check, stops it.
- `ith_jobs_benchmark` (CTest `jobs_benchmark`, label `benchmark`, run by CI on
  linux-clang-release) prints the empty-job overhead at 1, 4 and 8 workers, the speed-up of a
  1M-item `parallelFor`, continuation-chain latency and wake-up latency. It never fails on its
  numbers. ADR-0025's initial targets: under 200 ns per empty job at 8 workers, at least 5x
  speed-up on 8 threads.

```powershell
./scripts/dev.ps1 build -Config Release -Target ith_jobs_benchmark
build/windows/bin/Release/ith_jobs_benchmark.exe
```

## Not done yet

- Thread stack size and core masks in the thread layer (ADR-0025 rule 6); names and priority
  exist.
- Worker names through `ITH_PROFILE_THREAD_NAME` and a profiling zone per job (rule 18), once
  ADR-0007's profiling macros exist.
- Command-line overrides of the worker counts (rule 5): the application's job.
- A `JobSystemConfig` setting to run background workers at normal priority in tools, the cooker
  and tests (decided: revisit when the cooker exists).
- One wait epoch for all waiters: `notify_all` wakes every sleeping waiter on each counter
  completion and on each job queued that no idle worker picks up (a thundering herd). Cheap
  while waiters are few; per-counter or per-lane wake lists if profiles show otherwise.
- In configurations without workers, jobs nest on the waiting thread's stack without a depth
  limit, so deep job trees there can overflow the stack. Keep such trees shallow.
