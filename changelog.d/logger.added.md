- `ith/foundation/Log.h`: the engine logger. Subsystems define categories with
  `ITH_LOG_CATEGORY(Name)` and log with `ITH_LOG_TRACE` to `ITH_LOG_ERROR` and `std::format`
  strings checked at compile time. Each category has a runtime level, settable by name
  (`setLogCategoryLevel`); statements below it don't evaluate their arguments, and Trace and
  Debug statements are compiled out of shipping builds. Messages are formatted without
  allocating. Records go to installed sinks (`addLogSink`) in one order across threads; stderr is
  installed from the start. Failed asserts and `ITH_FATAL` are logged as Fatal records. See
  `docs/development/logging.md`.
- `ith/foundation/LogRingBuffer.h`: a sink that keeps the most recent records in memory, with
  sequence numbers, for views that read the log each frame.
- `ith_log_benchmark` measures disabled statements, formatted records and logging from several
  threads.
