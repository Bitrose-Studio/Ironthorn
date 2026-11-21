- `ith/jobs`: the job system. `ith::JobSystem` runs jobs on a foreground lane for frame work and
  a background lane for long work, each with its own named worker threads (background below
  normal OS priority), and on the main thread through `postToMainThread` and
  `runMainThreadJobs`. Jobs capture up to 48 bytes inline and foreground jobs never allocate.
  `JobCounter` and `wait` (which runs queued jobs while waiting, up to 8 nested waits),
  continuations with `submitAfter`, `parallelFor` with ranges that don't depend on the worker
  count, asserted wait rules and a deadlock watchdog, and a mode without workers for
  deterministic tests. `CancellationToken`, `Progress` and `FirstError` support long
  operations. See `docs/development/jobs.md`.
- `ith_jobs_benchmark` measures empty-job overhead, `parallelFor` speed-up, continuation latency
  and wake-up latency.
