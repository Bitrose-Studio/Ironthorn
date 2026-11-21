# ADR-0026: Job system: rules corrected after the first implementation

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** amends [ADR-0025](0025-job-system.md) (job system);
  [ADR-0007](0007-logging-and-profiling.md) (profiling macros),
  [ADR-0014](0014-ecs-storage-and-world.md) (benchmarks reported in CI),
  [ADR-0017](0017-runtime-and-editor-separation.md) (dedicated server)

## Context

The first `jobs` slice and its review found ADR-0025 rules that the implementation can't keep
as written, or that leave gaps: how a waiter sleeps, which lanes it helps, what happens when the
foreground pool is full of parked jobs, and when the watchdog fires. The implementation also
settled details ADR-0025 left open. This ADR changes only those rules. Everything else in
ADR-0025 stands.

## Options considered

- **Amend ADR-0025 by this ADR** (chosen), as ADR-0019 did for reflection: the accepted record
  stays readable and each change carries its reason.
- **Supersede ADR-0025 with a full rewrite.** Repeats nineteen rules for four corrections.
- **Keep the rules and change the code around them.** Sleeping on the counter (rule 12) can't
  wake a waiter to help, and a full pool held by parked continuations (rule 8) can't free a slot
  by running queued jobs; both rules fail as written.

## Decision

1. **Rule 12, sleeping: waiters sleep on one system-wide epoch, not on the counter.** A waiter
   with nothing to run sleeps with `std::atomic::wait` on a wait epoch owned by the
   `JobSystem`. The epoch moves when a counter reaches zero and when a job is queued that no
   idle worker will pick up; a sleeping waiter then re-checks its counter and the queues.
   - **Why:** a waiter sleeping on its counter wakes only when the counter is done, so it can't
     wake to help with a job queued meanwhile. And the job system never touches a counter after
     it has been seen done (the last decrement happens under the counter's mutex), so the
     caller may destroy it right after `wait` returns; sleeping on the counter itself would
     touch it after that point.
   - **Cost:** `notify_all` wakes every sleeping waiter on each completion and on each such
     queued job, though most go back to sleep (a thundering herd). Waiters are few today (the
     main thread and nested waits), so this is cheap.
2. **Rule 12, helping: the lanes a waiter helps are re-read on every pass, and lanes without
   workers are helped by any waiter.**
   - A counter can gain lanes while it is waited on (a running job submits to another lane with
     the same counter), so the waiter reads the counter's lanes again on every pass instead of
     once at the start.
   - A lane configured with zero workers (the deterministic zero-worker mode of rule 19, or a
     dedicated server with zero background workers) has nobody else to run its jobs. Any waiter
     runs them, at any depth, whatever lanes its counter waits for.
   - Help on the waiter's own lane keeps the nesting limit of 8.
   - Main-thread jobs stay restricted: run only on the main thread, outside any job, when the
     counter waits for them.
   - **Risk:** in zero-worker configurations jobs nest without a depth limit on the waiting
     thread's stack. Deep job trees there can overflow the stack; tests keep their trees
     shallow, and a server with zero background workers keeps its background work flat.
3. **Rule 8, pool full: the submitter stops instead of livelocking.** When the fixed foreground
   pool is full, the submitting thread still runs queued foreground jobs to free a record. When
   nothing is runnable because the records are held by parked continuations, no record can
   free; after about 1 s without progress the submitter stops with `ITH_FATAL`. The 4,096 size
   is a limit to design within, not a soft cap: frame work that parks thousands of
   continuations at once is restructured, or the application raises the setting.
   **Why:** spinning forever hides the bug; a fatal error with a message points at it.
4. **Rule 12, watchdog: fires only when every foreground thread is stuck.** It fires when every
   foreground worker **and** the main thread are asleep in waits past the helping depth while
   foreground jobs stay queued for about 1 s, then stops with `ITH_FATAL`. A main thread that is
   merely busy (not waiting) is not a deadlock, because it will reach a wait or the frame end
   and run the queued jobs. **Why:** the old wording counted only workers, which reported a
   deadlock whenever the main thread was busy for a second.
5. **Implementation choices ADR-0025 left open:**
   - **Queues:** one FIFO queue behind an `ith::Mutex` per worker, per lane. A worker pushes to
     and pops from its own queue and steals from the others when it runs dry; other threads
     push round-robin. **Why:** one queue per lane was measured at 1.4 µs per job and 4.4×
     speed-up at 8 workers, because every worker contended for one lock.
   - **The one lock-free part:** released job records go onto an atomic stack, so releasing
     (once per job, on every worker) never blocks. Only the side holding the pool mutex takes
     from it, and it takes the whole stack at once, so the stack has no ABA problem. Everything
     else uses mutexes, condition variables and `std::atomic::wait`.
   - **Measured** on the development machine (Ryzen 9 5950X, Windows, Release): an empty job
     costs 116–378 ns at 1–8 workers, against the report-only target of 200 ns at 8 workers
     (rule 19); a 1M-item `parallelFor` speeds up 7.5× on 8 threads, against the 5× target. The
     per-job target is missed at 8 workers; per rule 19 this is reported, not blocking, until the
     reference machines are measured.
6. **Not yet implemented** (follow-ups, not changes to the rules):
   - rule 6: thread stack size and core mask in the platform layer (name and priority exist);
   - rule 18: worker names through `ITH_PROFILE_THREAD_NAME` and a profiling zone per job, until
     ADR-0007's profiling macros exist. Workers are named through the OS meanwhile.

## Consequences

- A waiter wakes to help with newly queued work, and counters can live on the waiter's stack
  and die as soon as `wait` returns.
- Zero-worker tests and servers run every job inline, at the price of unbounded nesting on the
  waiting thread's stack.
- A foreground pool exhausted by parked continuations and a true foreground deadlock both end
  in a clear fatal error instead of a hang; a busy main thread no longer trips the watchdog.
- Follow-up work: tests for each rule above (lanes gained while waiting, zero-worker lanes
  helped by any waiter, the pool-full stop, the watchdog with a busy main thread); thread stack
  size and core mask; profiling names and zones when ADR-0007's macros land; the per-job
  overhead at 8 workers, measured again on the reference machines.
- **Revisit when:**
  - profiles show many sleeping waiters woken per completion (per-counter or per-lane wake
    lists instead of one epoch);
  - zero-worker configurations overflow a stack (a depth limit with a fallback there);
  - the per-job target is still missed on the reference machines (lock-free deques, per
    ADR-0025);
  - real frame work hits the foreground pool limit.
