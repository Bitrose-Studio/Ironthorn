# ADR-0025: The job system

- **Status:** Accepted
- **Amended by:** [ADR-0026](0026-job-system-rules-corrected.md)
- **Date:** 2026-11-21
- **Related:** amends [ADR-0012](0012-windowing-and-event-loop.md) rule 9 (main-thread queue);
  [ADR-0001](0001-cpp-language-and-safety-rules.md) (no exceptions, `ith::Expected`,
  thread-safety annotations, "parallel work runs through the job system", sanitizer jobs),
  [ADR-0004](0004-modules-and-layering.md) (`platform` layer holds threads and jobs),
  [ADR-0007](0007-logging-and-profiling.md) (profiling macros, Tracy),
  [ADR-0014](0014-ecs-storage-and-world.md) (serial executor first, parallel executor later),
  [ADR-0015](0015-asset-identity-and-reimport.md) (import, DDC, cooker),
  [ADR-0017](0017-runtime-and-editor-separation.md) (module kinds, dedicated server)

## Context

ADR-0001 sends all parallel work through a job system, and P1 has to deliver one. Its users are
already known:

- **Frame work, short and latency-bound:** the ECS parallel executor over declared access
  (ADR-0014 rule 18), render graph command recording on several threads (the RHI records command
  lists in parallel, ADR-0009 rule 12), later physics, animation and culling.
- **Long work, throughput-bound and sometimes blocking on files:** asset import and reimport, DDC
  reads and writes, shader compilation, the cooker, editor operations with progress and
  cancellation, and asset streaming in games.

Constraints:

- **No exceptions or RTTI** (ADR-0001). Failures travel as `ith::Expected`.
- **TSan and ASan run in CI** (ADR-0001 rule 8), so the scheduler must be checkable by them.
- **Every target needs jobs**, including the dedicated server and Shipping builds (ADR-0017), so
  the module is `runtime`, not `client` like `platform`.
- **Consoles later.** Their toolchains are Clang-based, and threads, priorities and core masks
  are platform-specific there; fibers are platform-specific everywhere.
- **Hardware:** the Steam Deck has 4 cores / 8 threads (ADR-0005). 6 and 8 cores are the most
  common desktops; editor machines have 16 or more.

Lessons:

- An earlier prototype wrapped a third-party scheduler behind `submit`, `parallelFor` and
  `wait`. The wrapper moved exceptions across threads, allocated a `std::function` per job,
  could only be driven from the thread that created it, and had no dependencies or counters.
  No engine code ever used it, and editor operations stayed blocking. A job system has to be
  shaped by its users, and long editor work needs its own path, or nothing gets moved off the
  main thread.
- Unreal's public documentation of its
  [Tasks System](https://dev.epicgames.com/documentation/en-us/unreal-engine/tasks-systems-in-unreal-engine)
  (which shares its scheduler and workers with the older Task Graph) describes
  prerequisites, nested tasks, events, pipes (serialized task chains) and five priorities split
  into foreground and background. It says waiting inside a task "should be avoided if possible,
  as it limits scalability". **UE 5.5 deprecated busy-waiting** (running other tasks while
  waiting) because it caused deadlocks, stutters and stack overflows, and replaced it with
  oversubscription: standby threads wake while a worker waits. It uses tasks, not fibers.
  Unreal's long work runs on background workers, separate from frame work.

## Options considered

Measured on the development machine (Ryzen 9 5950X, 16 cores / 32 threads, Windows, clang-cl
`/O2`), with a throwaway benchmark of the simplest design (one mutex-protected deque per worker,
owner pops at the back, thieves steal from the front):

| Measurement | Result |
|---|---|
| Uncontended `std::mutex` push and pop | 17.6 ns |
| `std::thread` create and join | 77 µs |
| Empty job, pushed by one thread, 1 / 4 / 32 workers | 55 / 84 / 984 ns |

A worker thread per task is far too expensive, a mutex queue is cheap until many thieves hit
one queue, and splitting work into a few ranges per worker (not one job per item) and putting
idle workers to sleep are what matter. Lock-free queues are an optimization for later.

1. **Own task scheduler on `std::thread`** (chosen). Per-worker queues with stealing, a
   global queue per lane, counters, continuations, `parallelFor`. Roughly 1–1.5k lines plus
   tests. Shaped for our users (lanes, progress, cancellation, main-thread queue), no
   exceptions, no allocation per frame job, and only standard atomics and mutexes, which TSan
   understands. We maintain and debug it.
2. **[enkiTS](https://github.com/dougbinks/enkiTS) v1.12** (zlib, released 2026-07-04, active),
   about 2.1k lines of C++11. No exceptions. Task sets with ranges, pinned tasks for named
   threads, dependencies, 3 priorities (up to 5 by macro), a custom allocator and profiler
   callbacks. Threads that aren't its own must be registered and counted in advance
   (`numExternalTaskThreads`). Priorities share one set of threads, so a long low-priority
   task holds a worker until it ends; separate foreground and background pools need two
   schedulers. The credible alternative: it would save the queue code but not the API layer,
   lanes, progress or cancellation, which we write either way.
3. **[Taskflow](https://github.com/taskflow/taskflow) v4.1.0** (MIT, 2026-06-20, C++20,
   about 34k lines of headers). A task-graph library built around exception propagation:
   `TF_DISABLE_EXCEPTION_HANDLING` removes its `try`/`catch` and turns internal errors into a
   message on `std::cerr` and `std::terminate`, but the core still uses `std::exception_ptr`,
   `std::future` and `std::promise`. A build with `-fno-exceptions` was not verified. Too large
   for what we need, and graph-first where our graphs (ECS schedule, render graph) are already
   our own.
4. **[oneTBB](https://github.com/uxlfoundation/oneTBB) v2023.2.0** (Apache-2.0, 2026-10-28).
   Mature (arenas, priorities, cancellation through task groups), but the runtime library is
   built with exceptions and, per the
   [specification](https://oneapi-spec.uxlfoundation.org/specifications/oneapi/latest/elements/onetbb/source/configuration/feature_macros),
   "may still throw an exception when `TBB_USE_EXCEPTIONS=0`". Conflicts with ADR-0001 rule 4,
   and large for a console port.
5. **Fibers** (own, or [marl](https://github.com/google/marl), Apache-2.0). A fiber can wait
   without blocking its thread or growing the stack, which is the one real advantage. marl is
   **archived** (read-only on GitHub, last commit 2026-04-27, no releases), so it is out. Own
   fibers need a context switch per platform (assembly or OS fibers; consoles differ again),
   explicit sanitizer annotations for every switch (`__sanitizer_start_switch_fiber`,
   `__tsan_switch_to_fiber`), fiber-aware profiling (Tracy's fiber mode), care with
   `thread_local` values and OS mutexes held across a switch, and debugger support. None of the
   listed users needs to suspend mid-job: frame work is fork-join, and long work that blocks on
   files runs on its own threads.
6. **Standard library only.** `std::async` and a thread per task cost 77 µs per task;
   parallel algorithms with `std::execution::par` depend on the implementation (the MSVC
   library uses the Windows thread pool, libstdc++ needs TBB) and give no control over
   priorities or threads; `std::execution` senders are C++26 and not in our toolchains.

## Decision

### Model

1. **Tasks on threads, no fibers.** A job is a callable run to completion on one thread.
   **Why:** it serves every listed user, and TSan, ASan, debuggers and Tracy work unchanged on
   all platforms.
2. **An own scheduler in a new module `jobs`** (layer `platform`, kind `runtime`, depends on
   `foundation` only). Headers under `ith/jobs/`, namespace `ith`. No third-party code. If the
   benchmarks (rule 19) miss their targets and a library is reconsidered, it needs a
   dependency-auditor verdict before adoption.
3. **No global instance.** The application creates one `JobSystem` at startup and passes it to
   the subsystems that use it (ECS executor, renderer, asset manager, editor). Tests create
   their own, with any worker count, including zero.

### Lanes and threads

4. **Two lanes with separate worker threads:**
   - **Foreground**, for frame work: short jobs (guideline under 1 ms), never blocking on files,
     sockets or OS waits other than job waits. Normal OS priority.
   - **Background**, for long work: import, cook, shader compilation, DDC, streaming, editor
     operations. Jobs may block on files. Below-normal OS priority, so the OS preempts them in
     favour of frame work.

   Foreground workers never run background jobs, so a two-second texture compression can't
   delay a frame. **Why:** jobs aren't preemptive, so priorities on one shared pool can't stop a
   long job from holding a frame worker; the OS can, between two sets of threads.
5. **Worker counts** default to foreground = hardware threads − 1 (the main thread takes part
   while it waits) and background = clamp(hardware threads / 4, 1, 4). Both are settings,
   overridable on the command line; a dedicated server can run with fewer, and consoles set
   counts and core masks per platform. The Steam Deck default is 7 + 2 workers.
6. **Thread creation goes through a small internal platform layer** in `jobs` (name, priority,
   core mask, stack size; `std::thread` with native calls on Windows and Linux). Console ports
   add a file there, not an API change.
7. **Main-thread jobs.** `postToMainThread(job)` queues a job that the main thread runs in
   `runMainThreadJobs()`, called once per frame by the application. Background results that
   touch main-thread state (asset database, editor UI, windows) come back this way. **This
   amends ADR-0012 rule 9:** the thread-safe queue for posting work to the main thread is this
   one, in `jobs`, not a separate queue in `platform`, so the dedicated server has it too. No other thread affinity exists; a
   render thread, if the renderer ADR wants one, gets its own queue the same way.

### Jobs, dependencies and waiting

8. **A job is a name and an inline callable.** The name is a string literal (for profiling and
   debugging). The callable is stored inline (captures up to 48 bytes; larger captures are a
   compile error, so callers point at their own data). **Foreground jobs never allocate:**
   their records come from a fixed pool (default 4,096 slots); when it is full, the submitting
   thread runs queued foreground jobs until a slot frees. Background job records come from a
   growable pool, because that lane is not a hot path.
9. **Counters for dependencies.** A `JobCounter` is owned by the caller, not movable, and counts
   unfinished jobs submitted with it. Destroying a counter with work pending, waiters or
   continuations is an assert.
10. **Continuations.** `submitAfter(counter, job)` queues a job when the counter reaches zero.
    Chains (import, then DDC write, then a main-thread database update) are built this way
    without blocking any thread. There is no general task-graph builder: the ECS executor and
    the render graph build their own graphs from counters and continuations.
11. **`parallelFor(name, count, batchSize, body)`** splits `[0, count)` into ranges claimed
    through a shared atomic cursor. The caller claims ranges too, so it only waits for ranges
    already running. Range boundaries depend on `count` and `batchSize` only, never on the
    worker count.
12. **Waiting.** `wait(counter)` may be called from the main thread, a registered thread or a
    job. While the counter is non-zero, the waiter runs queued jobs **of its own lane** (the
    main thread counts as foreground), up to a nesting depth of 8; beyond that, or when nothing
    is queued, it sleeps on the counter (`std::atomic::wait`). Rules, checked by asserts in
    Debug and Development builds:
    - no wait while holding an `ith::Mutex` (it gains a per-thread held count);
    - a foreground job never waits on a background counter, and the main thread does so only
      outside the frame loop (shutdown, command-line tools such as the cooker); otherwise poll
      the counter or use a continuation;
    - no job waits on a main-thread job;
    - a watchdog reports a deadlock when every foreground thread sleeps in `wait` while
      foreground jobs are queued, then stops with `ITH_FATAL`.

    Continuations are preferred inside jobs, as Unreal's documentation advises.
    **Why not Unreal's oversubscription:** standby threads are more machinery than the bounded
    helping above needs; they become the fix if the watchdog or profiles show stalls.
13. **External threads** (a future render thread, an audio callback thread) may submit jobs
    and wait without registration limits.

### Cancellation, progress and errors

14. **Cancellation is cooperative.** A `CancellationToken` is an atomic flag owned by the
    operation; jobs check it and return early. Queued jobs are not removed; they run, see the
    flag and return.
15. **Progress** is an atomic done/total pair that jobs update and the editor reads once per
    frame. The editor's progress UI, operation list and cancel buttons live in editor modules.
16. **No exceptions.** Jobs return nothing. Results, including `ith::Expected` errors, are
    written into storage owned by the caller. A small operation helper keeps the first error
    of a group of jobs.

### Determinism, threads and tools

17. **Execution order is not deterministic; results must be.** Systems that need deterministic
    output (gameplay simulation, ECS commands, cooking) get it from independent work and fixed
    merge order: per-range or per-system buffers combined in index order. Cooking the same
    input twice gives identical output whatever the worker count, and a test checks it.
18. **Profiling and sanitizers.** Each worker is named with `ITH_PROFILE_THREAD_NAME`; each job
    runs in a profiling zone with its name (ADR-0007; a named-zone macro is added there if
    needed); queue depths can be plotted. The scheduler uses standard atomics, mutexes and
    `std::atomic::wait` only, and **no standalone `std::atomic_thread_fence`**, which TSan does
    not model ([GCC](https://gcc.gnu.org/pipermail/gcc-bugs/2021-September/756461.html) warns
    about it under `-fsanitize=thread`, and a matching
    [Clang warning](https://lists.llvm.org/pipermail/cfe-commits/Week-of-Mon-20251103/771602.html)
    was proposed in 2025). Shared state uses `ith::Mutex` with thread-safety annotations.
    A lock-free work-stealing deque (Chase-Lev) replaces the mutex queues only if benchmarks
    show the need, and must then pass the TSan stress tests.

### Evidence

19. **Tests and benchmarks** ship with the module:
    - unit tests with zero workers (everything runs inline in `wait`, so tests are
      deterministic) and with several;
    - stress tests: random job trees with counters, continuations, nested waits, cancellation
      and pool exhaustion, millions of jobs in normal builds and a shorter run in the TSan and
      ASan jobs;
    - the deadlock watchdog, the wait rules and shutdown with pending jobs (shutdown cancels
      background work and drains both lanes);
    - benchmarks reported in CI, like the ECS benchmarks (ADR-0014 rule 19): empty-job
      overhead, `parallelFor` over 1M items from 1 to N workers, continuation-chain latency
      and wake-up latency. Initial targets: under 200 ns per job at 8 workers and at least 5×
      speed-up of a 1M-item `parallelFor` on 8 cores. A missed target is reported but doesn't
      block a merge until the reference machines (ADR-0005) are measured and the targets
      reviewed.

## Consequences

- One small scheduler with no dependency, built for frame work and for long editor work alike;
  the editor can move import, cook and shader compilation off the main thread from the start.
- We own the scheduler's correctness. The TSan stress tests and the wait-rule asserts are what
  keep it honest.
- Jobs can't suspend mid-way. Long work that waits on files sits on background threads, which
  is why that lane exists and is sized separately.
- Help-while-waiting can still stall a frame when a waiter picks up a slow foreground job; the
  under-1 ms guideline and profiles catch it.
- Follow-up work:
  - the `jobs` module, its tests and benchmarks (P1);
  - ADR-0012's main-thread queue implemented as rule 7, used by `platform`;
  - the per-thread held-lock count in `ith::Mutex` (rule 12);
  - a named-zone profiling macro if ADR-0007's set lacks one;
  - the ECS parallel executor, render graph recording, async import and the cooker built on it;
  - an adapter for physics middleware's job interface when physics lands.
- **Owner's answers at acceptance (2026-11-21):** two lanes with separate workers, background
  at lower OS priority, default counts as in rule 5 (settings); no fibers until profiles show
  the need; bounded help-while-waiting as in rule 12; an explicit `JobSystem` passed to
  subsystems, no global accessor; our own scheduler now, enkiTS only after a dependency audit
  if benchmarks miss their targets; benchmark targets reported in CI without blocking merges
  until the reference machines are measured.
- **Revisit when:**
  - profiles show frame workers idle in waits on the critical path (oversubscription, then
    fibers);
  - benchmarks miss their targets (lock-free deques, or enkiTS after a dependency audit);
  - a console needs thread control the platform layer of rule 6 can't give;
  - async file I/O lands and background threads no longer need to block.
