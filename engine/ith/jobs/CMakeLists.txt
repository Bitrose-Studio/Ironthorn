# The job system (ADR-0025): foreground and background worker lanes, job counters,
# continuations, parallelFor, help-while-waiting with a deadlock watchdog, the main-thread job
# queue, cancellation, progress and first-error helpers. A runtime module: every process,
# the dedicated server included, runs jobs. Standard library only.
find_package(Threads REQUIRED)

ith_add_module(jobs
    LAYER platform
    SOURCES
        CancellationToken.h
        FirstError.h
        Job.h
        JobCounter.h
        JobCounter.cpp
        JobSystem.h
        JobSystem.cpp
        Progress.h
        Thread.h
        Thread.cpp
        Watchdog.h
    DEPENDS
        foundation
    PRIVATE_LIBRARIES
        Threads::Threads)
