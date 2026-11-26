#pragma once

// JobSystem: the engine's scheduler for parallel work (ADR-0025).
//
//   ith::JobSystem jobs;                       // on the main thread, at startup
//
//   ith::JobCounter counter;
//   jobs.submit(ith::JobLane::Foreground, "Animate", [&] { animate(); }, &counter);
//   jobs.parallelFor("Cull", objects.size(), 256, [&](std::size_t begin, std::size_t end) {
//       cullRange(begin, end);
//   });
//   jobs.wait(counter);
//
//   jobs.submit(ith::JobLane::Background, "Compress", [this] {
//       compress();
//       m_jobs.postToMainThread("Publish", [this] { publish(); });
//   });
//   ...
//   jobs.runMainThreadJobs();                  // once per frame
//
// There is no global instance: the application creates one JobSystem and passes it to the
// subsystems that use it.

#include "ith/foundation/Assert.h"
#include "ith/jobs/Job.h"
#include "ith/jobs/JobCounter.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace ith {

// How deep waits may nest on one thread and still run other jobs while they wait. A deeper
// wait sleeps until its counter is done (ADR-0025 rule 12).
inline constexpr int kMaxHelpingWaitDepth = 8;

// The default number of foreground workers: hardware threads - 1, because the main thread runs
// foreground jobs while it waits. 0 on a single hardware thread.
std::uint32_t defaultForegroundWorkerCount();

// The default number of background workers: hardware threads / 4, clamped to 1-4.
std::uint32_t defaultBackgroundWorkerCount();

struct JobSystemConfig {
    // Worker threads per lane. 0 is valid: a lane without workers runs its jobs on the threads
    // that wait for them, which makes tests deterministic.
    std::uint32_t foregroundWorkers = defaultForegroundWorkerCount();
    std::uint32_t backgroundWorkers = defaultBackgroundWorkerCount();

    // Records of foreground jobs that are queued or parked as continuations; a running job no
    // longer holds one. Fixed, so frame work never allocates. Must be at least 1.
    std::uint32_t foregroundJobCapacity = 4096;
};

// Runs jobs on two lanes of worker threads (foreground at normal OS priority, background below
// normal) and on the main thread.
//
// The thread that constructs the JobSystem is its main thread. All members are thread-safe
// unless they say otherwise; jobs may submit and wait from inside other jobs. A job is a
// callable taking no arguments and returning nothing, with captures of at most kJobCaptureSize
// bytes (larger ones don't compile). Its name, for debuggers and the profiler, must be a non-empty
// string literal or another string that outlives the job (ADR-0025 rule 8).
//
// Destruction (main thread, outside any job) asks for shutdown (shutdownRequested() turns true,
// so long background jobs can stop early), then runs or waits for every queued, running and
// continuation job of every lane, main-thread jobs included, and joins the workers. Jobs may
// still submit during shutdown; no job is dropped. Threads other than the main thread and the
// jobs must have stopped using the JobSystem by then.
class JobSystem {
public:
    explicit JobSystem(const JobSystemConfig& config = {});
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem(JobSystem&&) = delete;
    JobSystem& operator=(JobSystem&&) = delete;

    // Queues `function` on `lane`. With a counter, the counter counts the job until it has run.
    // A foreground submit never allocates: when every foreground record is in use, the calling
    // thread runs queued foreground jobs until one frees, so it must not hold a lock those jobs
    // take (asserted for ith::Mutex). When no record frees and nothing is runnable for about a
    // second (records held by parked continuations), it stops with ITH_FATAL (ADR-0026 rule 3).
    template <typename F>
    void submit(JobLane lane, const char* name, F&& function, JobCounter* counter = nullptr) {
        detail::Job* job = makeJob(lane, name, std::forward<F>(function), counter);
        enqueue(job);
    }

    // Queues `function` on `lane` once `after` is done; at once if it already is. `counter`, if
    // given, counts the job from this call on. `after` must stay alive until the job is queued,
    // which the usual wait on it, or on `counter`, ensures. `counter` must not be `after`
    // (asserted): the counter could never reach zero.
    template <typename F>
    void submitAfter(JobCounter& after, JobLane lane, const char* name, F&& function, JobCounter* counter = nullptr) {
        detail::Job* job = makeJob(lane, name, std::forward<F>(function), counter);
        enqueueAfter(after, job);
    }

    // Queues `function` for the main thread, which runs it in runMainThreadJobs(). Results of
    // background work that touch main-thread state come back this way.
    template <typename F>
    void postToMainThread(const char* name, F&& function, JobCounter* counter = nullptr) {
        submit(JobLane::MainThread, name, std::forward<F>(function), counter);
    }

    // Calls body(begin, end) for consecutive ranges covering [0, count), of batchSize items
    // except perhaps the last, on the foreground lane and on the calling thread, and returns
    // when every range is done. Range boundaries depend only on count and batchSize, never on
    // the worker count; the order ranges run in is not defined, so a deterministic result
    // comes from per-range outputs merged in range order. Waits like wait(), with its rules.
    // Precondition: batchSize > 0.
    template <typename Body>
    void parallelFor(const char* name, std::size_t count, std::size_t batchSize, Body&& body) {
        using BodyType = std::remove_reference_t<Body>;
        ParallelFor work{
            .count = count,
            .batchSize = batchSize,
            .body = const_cast<void*>(static_cast<const void*>(&body)),
            .call = [](void* target, std::size_t begin,
                       std::size_t end) { (*static_cast<BodyType*>(target))(begin, end); },
        };
        runParallelFor(name, work);
    }

    // Returns when `counter` is done (ADR-0025 rule 12 as corrected by ADR-0026). While it
    // isn't, the calling thread runs queued jobs of its own lane (the main thread counts as
    // foreground) up to kMaxHelpingWaitDepth nested waits, jobs of foreground and background
    // lanes without workers at any depth, and, on the main thread outside any job, main-thread
    // jobs the counter waits for; the counter's lanes are read again on every pass. Otherwise it
    // sleeps until the counter is done or a job it may run is queued.
    // Asserted rules: no wait while holding an ith::Mutex; a foreground job never waits on a
    // counter of background jobs; a job never waits on a counter of main-thread jobs. A
    // watchdog stops the process with ITH_FATAL when every foreground worker and the main
    // thread sleep in waits past the helping depth while foreground jobs stay queued for a
    // second. Nested waits inside jobs stack up on helping threads, so prefer continuations.
    void wait(JobCounter& counter);

    // Registers `hook`, called with `context` after every main-thread post (postToMainThread, and
    // main-thread continuations as they are queued) and every update of a Progress made with
    // this JobSystem, from the thread that posts (ADR-0035 rule 23). The application connects it to
    // Platform::wake(), so an event wait ends when a result is ready:
    //
    //   jobs.setMainThreadWake([](void* /*context*/) { ith::Platform::wake(); }, nullptr);
    //
    // The hook must be thread-safe and must not block, post jobs or call this function. A null hook
    // removes it. Calls are serialized with this function: once it returns, no call of the previous
    // hook is running, so the application can remove the hook before destroying what it calls.
    void setMainThreadWake(void (*hook)(void*), void* context);

    // Calls the main-thread wake hook, if one is set. Thread-safe.
    void wakeMainThread() const;

    // Runs the main-thread jobs queued before the call, in submission order, and returns how
    // many ran. Jobs they post run on the next call. Main thread only, outside any job.
    // The application's loop calls it once per frame, after pumpEvents() or waitEvents() and
    // before it updates and renders (ADR-0035 rule 22); a server calls it at the start of its tick.
    std::uint32_t runMainThreadJobs();

    // The number of worker threads of `lane` (0 for JobLane::MainThread).
    std::uint32_t workerCount(JobLane lane) const;

    // True on the thread that created the JobSystem.
    bool isMainThread() const;

    // True once destruction began. Long background jobs should check it and stop early.
    bool shutdownRequested() const;

private:
    struct State;

    struct ParallelFor {
        std::size_t count = 0;
        std::size_t batchSize = 0;
        void* body = nullptr;
        void (*call)(void* body, std::size_t begin, std::size_t end) = nullptr;
    };

    template <typename F>
    detail::Job* makeJob(JobLane lane, const char* name, F&& function, JobCounter* counter) {
        using Function = std::decay_t<F>;
        static_assert(std::is_invocable_v<Function&> && std::is_void_v<std::invoke_result_t<Function&>>,
                      "A job is a callable that takes no arguments and returns nothing");
        static_assert(sizeof(Function) <= kJobCaptureSize,
                      "Job captures are limited to kJobCaptureSize (48) bytes: capture a pointer to your own data");
        static_assert(alignof(Function) <= alignof(std::max_align_t), "Job callables must not be over-aligned");
        ITH_ASSERT_MSG(name != nullptr && name[0] != '\0', "A job needs a non-empty name");
        detail::Job* job = acquireJob(lane, name, counter);
        ::new (static_cast<void*>(job->storage.data())) Function(std::forward<F>(function));
        job->relocate = [](void* from, void* to) {
            Function* source = std::launder(static_cast<Function*>(from));
            ::new (to) Function(std::move(*source));
            source->~Function();
        };
        job->run = [](void* storage) {
            Function* callable = std::launder(static_cast<Function*>(storage));
            (*callable)();
            callable->~Function();
        };
        return job;
    }

    detail::Job* acquireJob(JobLane lane, const char* name, JobCounter* counter);
    void enqueue(detail::Job* job);
    void enqueueAfter(JobCounter& after, detail::Job* job);
    void runParallelFor(const char* name, ParallelFor& work);

    std::unique_ptr<State> m_state;
};

} // namespace ith
