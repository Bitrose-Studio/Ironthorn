#include "ith/jobs/JobSystem.h"

#include "ith/foundation/Assert.h"
#include "ith/foundation/Compiler.h"
#include "ith/foundation/Mutex.h"
#include "ith/jobs/Thread.h"
#include "ith/jobs/Watchdog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// The design follows ADR-0025. In short:
//
// - Job records come from two pools: a fixed one for foreground jobs and a growable one for
//   background and main-thread jobs. A record links into one intrusive list at a time, and a
//   job leaves its record when it starts running.
// - Each lane has a FIFO queue per worker behind an ith::Mutex; workers take from each other's
//   queues when their own is empty. Idle workers spin briefly, then sleep on the lane's
//   condition variable.
// - Threads waiting on a counter run queued jobs they are allowed to (see wait()), or sleep on
//   the system's wait epoch with std::atomic::wait. The epoch moves when a counter reaches zero
//   and when a job is queued that no idle worker will pick up, so sleeping waiters re-check.
// - A counter's decrement to zero happens under the counter's mutex, and the counter is never
//   touched after that mutex is released, so a waiter that sees zero under the mutex may
//   destroy it.

namespace ith {
namespace {

using detail::Job;

constexpr std::size_t kLaneCount = 3;
// Keeps data written by different threads on different cache lines.
constexpr std::size_t kCacheLine = 64;
constexpr std::uint32_t kBackgroundPoolChunk = 256;
// How long the watchdog waits for a suspected deadlock to clear before it stops the process.
constexpr std::chrono::milliseconds kWatchdogGrace{1000};
constexpr std::chrono::milliseconds kWatchdogPoll{10};
// How long a submitter may find no free foreground record and nothing to run before it stops.
constexpr std::chrono::milliseconds kPoolExhaustedGrace{1000};
// Polls of its queue an idle worker makes before it sleeps.
constexpr int kIdleSpins = 2000;
// How often, in spins, a spinning worker looks at the other workers' queues.
constexpr int kStealInterval = 32;
// Rounds without work the main thread yields, rather than sleeps, while shutdown drains.
constexpr int kShutdownSpins = 10000;

std::size_t laneIndex(JobLane lane) {
    return static_cast<std::size_t>(lane);
}

std::uint8_t laneBit(JobLane lane) {
    return static_cast<std::uint8_t>(1u << laneIndex(lane));
}

// What the current thread is, for the JobSystem whose worker it is, and what it runs.
struct ThreadState {
    const JobSystem* workerOf = nullptr; // Set on worker threads only.
    JobLane workerLane = JobLane::Foreground;
    std::uint32_t workerIndex = 0;
    std::optional<JobLane> currentJob; // The lane of the innermost job running on this thread.
    int waitDepth = 0;                 // Nested waits (and pool-full help) on this thread.
};

thread_local ThreadState g_thread;

// Fixed or growable storage of job records. Records are taken from a free list under a mutex;
// released records go onto a lock-free stack that the taking side empties into the free list
// when it runs dry, so releasing (on every worker, once per job) never blocks. Only the side
// holding the mutex takes from the stack, and it takes all of it at once, so the stack has no
// ABA problem.
class JobPool {
public:
    JobPool(std::uint32_t capacity, bool growable) : m_chunkSize(capacity), m_growable(growable) {
        const MutexLock lock(m_mutex);
        addChunk();
    }

    // A free record, or nullptr when a fixed pool is exhausted.
    Job* tryAcquire() {
        const MutexLock lock(m_mutex);
        if (m_free == nullptr) {
            m_free = m_released.exchange(nullptr, std::memory_order_acquire);
        }
        if (m_free == nullptr) {
            if (!m_growable) {
                return nullptr;
            }
            addChunk();
        }
        Job* job = m_free;
        m_free = job->next;
        job->next = nullptr;
        m_acquisitions.store(m_acquisitions.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        return job;
    }

    // How many records were handed out so far. Changes while records are recycled, so a submitter
    // waiting for a free record can tell a busy pool from a stuck one. Read without the lock.
    std::uint64_t acquisitions() const { return m_acquisitions.load(std::memory_order_relaxed); }

    // True when every record is free. Only for shutdown: it walks the free lists.
    bool allFree() {
        const MutexLock lock(m_mutex);
        std::size_t count = 0;
        for (const Job* job = m_free; job != nullptr; job = job->next) {
            ++count;
        }
        for (const Job* job = m_released.load(std::memory_order_acquire); job != nullptr; job = job->next) {
            ++count;
        }
        return count == m_chunks.size() * m_chunkSize;
    }

    void release(Job* job) {
        Job* head = m_released.load(std::memory_order_relaxed);
        do {
            job->next = head;
        } while (!m_released.compare_exchange_weak(head, job, std::memory_order_release, std::memory_order_relaxed));
    }

private:
    void addChunk() ITH_REQUIRES(m_mutex) {
        std::vector<Job> chunk(m_chunkSize);
        for (std::uint32_t index = 0; index < m_chunkSize; ++index) {
            chunk[index].next = m_free;
            m_free = &chunk[index];
        }
        m_chunks.push_back(std::move(chunk));
    }

    Mutex m_mutex;
    std::vector<std::vector<Job>> m_chunks ITH_GUARDED_BY(m_mutex);
    Job* m_free ITH_GUARDED_BY(m_mutex) = nullptr;
    std::atomic<Job*> m_released{nullptr};
    std::atomic<std::uint64_t> m_acquisitions{0}; // Written under m_mutex.
    const std::uint32_t m_chunkSize;
    const bool m_growable;
};

// MSVC warns (C4324) that the queues and lanes below are padded; the padding is their point.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

// A FIFO queue of jobs behind a mutex, on cache lines of its own.
class alignas(kCacheLine) JobQueue {
public:
    void push(Job* job) {
        const MutexLock lock(m_mutex);
        job->next = nullptr;
        if (m_tail == nullptr) {
            m_head = job;
        } else {
            m_tail->next = job;
        }
        m_tail = job;
        m_size.store(m_size.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    // The oldest job, or nullptr. Skips the lock when the queue looks empty.
    Job* tryPop() {
        if (m_size.load(std::memory_order_relaxed) == 0) {
            return nullptr;
        }
        const MutexLock lock(m_mutex);
        Job* job = m_head;
        if (job != nullptr) {
            m_head = job->next;
            if (m_head == nullptr) {
                m_tail = nullptr;
            }
            job->next = nullptr;
            m_size.store(m_size.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
        }
        return job;
    }

    std::uint32_t size() const { return m_size.load(std::memory_order_relaxed); }

    // Every job, oldest first, linked through Job::next. Returns how many through `count`.
    Job* takeAll(std::uint32_t& count) {
        const MutexLock lock(m_mutex);
        Job* jobs = m_head;
        count = m_size.load(std::memory_order_relaxed);
        m_head = nullptr;
        m_tail = nullptr;
        m_size.store(0, std::memory_order_relaxed);
        return jobs;
    }

private:
    Mutex m_mutex;
    Job* m_head ITH_GUARDED_BY(m_mutex) = nullptr;
    Job* m_tail ITH_GUARDED_BY(m_mutex) = nullptr;
    // The length, read without the lock only as a hint.
    std::atomic<std::uint32_t> m_size{0};
};

// One lane: a queue per worker (one when it has none), its workers and, for the foreground
// lane, the watchdog's counts. A worker pushes to and pops from its own queue first and takes
// from the others when it is empty; other threads push round-robin. Per-worker queues keep
// the workers from contending for one lock.
//
// Sleeping without missing a job: a thread about to sleep increments a counter of sleepers
// (idleWorkers, or State::sleepingWaiters) and then looks at the queue sizes; a pusher pushes
// and then reads that counter with a read-modify-write. Both read-modify-writes are on the same
// atomic, so they are ordered: either the pusher sees the sleeper, or the sleeper's increment
// comes after the pusher's and sees its push.
struct Lane {
    std::uint32_t workerCount = 0; // Fixed after construction.
    std::uint32_t queueCount = 0;  // max(workerCount, 1)
    std::deque<JobQueue> queues;   // A deque: its elements never move.
    alignas(kCacheLine) std::atomic<std::uint32_t> nextQueue{0};
    alignas(kCacheLine) std::atomic<std::uint32_t> idleWorkers{0};

    alignas(kCacheLine) Mutex mutex; // Guards sleeping, stopping and the watchdog's counts.
    std::condition_variable_any wake;
    bool stopping ITH_GUARDED_BY(mutex) = false;
    std::uint32_t stuckWorkers ITH_GUARDED_BY(mutex) = 0; // Foreground only.
    bool mainThreadStuck ITH_GUARDED_BY(mutex) = false;   // Foreground only.

    std::vector<std::thread> workers;

    void init(std::uint32_t count) {
        workerCount = count;
        queueCount = std::max(count, 1u);
        for (std::uint32_t queue = 0; queue < queueCount; ++queue) {
            queues.emplace_back();
        }
    }

    void push(Job* job, std::uint32_t queue) { queues[queue % queueCount].push(job); }

    // A job from the queues, looking at `first` first; only there when `steal` is false.
    Job* pop(std::uint32_t first, bool steal = true) {
        const std::uint32_t tries = steal ? queueCount : 1;
        for (std::uint32_t offset = 0; offset < tries; ++offset) {
            if (Job* job = queues[(first + offset) % queueCount].tryPop()) {
                return job;
            }
        }
        return nullptr;
    }

    // Jobs in the queues; exact only when nobody pushes or pops.
    std::uint32_t queued() const {
        std::uint32_t total = 0;
        for (std::uint32_t queue = 0; queue < queueCount; ++queue) {
            total += queues[queue].size();
        }
        return total;
    }
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace

std::uint32_t defaultForegroundWorkerCount() {
    const std::uint32_t threads = std::max(1u, std::thread::hardware_concurrency());
    return threads - 1;
}

std::uint32_t defaultBackgroundWorkerCount() {
    const std::uint32_t threads = std::max(1u, std::thread::hardware_concurrency());
    return std::clamp(threads / 4, 1u, 4u);
}

struct JobSystem::State {
    explicit State(JobSystem& owner, const JobSystemConfig& config)
        : system(owner),
          foregroundPool(config.foregroundJobCapacity, false),
          backgroundPool(kBackgroundPoolChunk, true),
          mainThread(std::this_thread::get_id()) {
        lane(JobLane::Foreground).init(config.foregroundWorkers);
        lane(JobLane::Background).init(config.backgroundWorkers);
        lane(JobLane::MainThread).init(0);
    }

    JobSystem& system;
    JobPool foregroundPool;
    JobPool backgroundPool; // Background and main-thread jobs.
    std::array<Lane, kLaneCount> lanes;
    const std::thread::id mainThread;
    std::atomic<bool> shutdownRequested{false};

    // Sleeping waiters wait on the epoch; sleepingWaiters lets signalWaiters skip the notify
    // when nobody sleeps. A waiter increments sleepingWaiters and then checks its condition; a
    // signaller makes the condition true and then reads sleepingWaiters with a
    // read-modify-write, so at least one of them sees the other (as for idle workers, see Lane).
    std::atomic<std::uint32_t> waitEpoch{0};
    std::atomic<std::uint32_t> sleepingWaiters{0};

    Lane& lane(JobLane id) { return lanes[laneIndex(id)]; }

    JobPool& pool(JobLane id) { return id == JobLane::Foreground ? foregroundPool : backgroundPool; }

    bool isWorker() const { return g_thread.workerOf == &system; }

    bool isForegroundWorker() const { return isWorker() && g_thread.workerLane == JobLane::Foreground; }

    void signalWaiters() {
        if (sleepingWaiters.fetch_add(0) > 0) {
            waitEpoch.fetch_add(1);
            waitEpoch.notify_all();
        }
    }

    void enqueue(Job* job) {
        Lane& target = lane(job->lane);
        const bool ownLane = isWorker() && g_thread.workerLane == job->lane;
        target.push(job, ownLane ? g_thread.workerIndex : target.nextQueue.fetch_add(1, std::memory_order_relaxed));
        if (target.idleWorkers.fetch_add(0) > 0) {
            // The sleeping worker holds the lane mutex from its last check until it waits, so taking
            // the mutex here makes sure the notification reaches it.
            { const MutexLock lock(target.mutex); }
            target.wake.notify_one();
        } else {
            // Every worker is busy (or the lane has none): a waiter that can run the job may be
            // asleep.
            signalWaiters();
        }
    }

    void run(Job* job) {
        // The callable moves to this stack and the record goes back to its pool before the job
        // runs, so a full pool never waits on running jobs, only on queued or parked ones.
        // Not zeroed: relocate move-constructs the callable into it.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        alignas(std::max_align_t) std::array<std::byte, kJobCaptureSize> callable;
        const JobLane jobLane = job->lane;
        JobCounter* counter = job->counter;
        void (*const invoke)(void*) = job->run;
        job->relocate(job->storage.data(), callable.data());
        pool(jobLane).release(job);

        const std::optional<JobLane> outer = g_thread.currentJob;
        g_thread.currentJob = jobLane;
        invoke(callable.data());
        g_thread.currentJob = outer;
        if (counter != nullptr) {
            finishOne(*counter);
        }
    }

    // Counts one job of `counter` as done; the last one queues the continuations.
    void finishOne(JobCounter& counter) {
        std::uint32_t pending = counter.m_pending.load();
        while (pending > 1) {
            if (counter.m_pending.compare_exchange_weak(pending, pending - 1)) {
                return;
            }
        }
        Job* continuations = nullptr;
        {
            const MutexLock lock(counter.m_mutex);
            const std::uint32_t previous = counter.m_pending.fetch_sub(1);
            ITH_ASSERT_MSG(previous != 0, "JobCounter finished more jobs than were submitted with it");
            if (previous != 1) {
                return; // A job was added to the counter meanwhile.
            }
            continuations = counter.m_continuations;
            counter.m_continuations = nullptr;
        }
        // From here on the counter may be gone. Continuations were parked newest first.
        Job* ordered = nullptr;
        while (continuations != nullptr) {
            Job* next = continuations->next;
            continuations->next = ordered;
            ordered = continuations;
            continuations = next;
        }
        while (ordered != nullptr) {
            Job* next = ordered->next;
            enqueue(ordered);
            ordered = next;
        }
        signalWaiters();
    }

    struct HelpLanes {
        bool foreground = false;
        bool background = false;
        bool mainThread = false;
    };

    // Which lanes the calling thread may run while it waits on a counter used with
    // `counterLanes` (ADR-0026 rule 2): its own lane while within the helping depth; foreground
    // and background lanes without workers at any depth, because nobody else would run them;
    // and, on the main thread outside any job, main-thread jobs when the counter counts some. A
    // counter can gain lanes while it is waited on, so waiters ask again on every pass.
    HelpLanes helpLanes(std::uint8_t counterLanes, bool withinDepth) const {
        const bool onMainThread = std::this_thread::get_id() == mainThread;
        const bool worker = isWorker();
        const auto mayRun = [&](JobLane id) {
            const bool own = worker ? g_thread.workerLane == id : (onMainThread && id == JobLane::Foreground);
            const bool orphan = lanes[laneIndex(id)].workerCount == 0;
            return (own && withinDepth) || orphan;
        };
        return HelpLanes{
            .foreground = mayRun(JobLane::Foreground),
            .background = mayRun(JobLane::Background),
            .mainThread =
                onMainThread && !g_thread.currentJob.has_value() && (counterLanes & laneBit(JobLane::MainThread)) != 0,
        };
    }

    // A job of lane `id` for the calling thread; a worker of that lane looks at its own queue first.
    Job* popFrom(JobLane id) {
        Lane& target = lane(id);
        const bool own = isWorker() && g_thread.workerLane == id;
        return target.pop(own ? g_thread.workerIndex : target.nextQueue.load(std::memory_order_relaxed));
    }

    Job* popHelpable(const HelpLanes& help) {
        Job* job = nullptr;
        if (help.foreground) {
            job = popFrom(JobLane::Foreground);
        }
        if (job == nullptr && help.background) {
            job = popFrom(JobLane::Background);
        }
        if (job == nullptr && help.mainThread) {
            job = popFrom(JobLane::MainThread);
        }
        return job;
    }

    bool anyQueued(const HelpLanes& help) {
        return (help.foreground && lane(JobLane::Foreground).queued() > 0) ||
               (help.background && lane(JobLane::Background).queued() > 0) ||
               (help.mainThread && lane(JobLane::MainThread).queued() > 0);
    }

    static detail::ForegroundWaitState foregroundWaitState(Lane& foreground) ITH_REQUIRES(foreground.mutex) {
        return detail::ForegroundWaitState{
            .workers = foreground.workerCount,
            .stuckWorkers = foreground.stuckWorkers,
            .mainThreadStuck = foreground.mainThreadStuck,
            .queuedJobs = foreground.queued(),
        };
    }

    // Registers the calling foreground thread as unable to run jobs, and stops the process if
    // no foreground thread can run the queued foreground jobs for kWatchdogGrace.
    void enterStuckSleep(const JobCounter& counter, bool worker) {
        Lane& foreground = lane(JobLane::Foreground);
        bool suspect = false;
        {
            const MutexLock lock(foreground.mutex);
            if (worker) {
                ++foreground.stuckWorkers;
            } else {
                foreground.mainThreadStuck = true;
            }
            suspect = detail::isForegroundDeadlock(foregroundWaitState(foreground));
        }
        const auto deadline = std::chrono::steady_clock::now() + kWatchdogGrace;
        while (suspect && counter.m_pending.load() != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                ITH_FATAL("Job system deadlock: every foreground worker and the main thread are waiting past the "
                          "helping depth while foreground jobs stay queued. Use continuations instead of nested "
                          "waits.");
            }
            std::this_thread::sleep_for(kWatchdogPoll);
            const MutexLock lock(foreground.mutex);
            suspect = detail::isForegroundDeadlock(foregroundWaitState(foreground));
        }
    }

    void leaveStuckSleep(bool worker) {
        Lane& foreground = lane(JobLane::Foreground);
        const MutexLock lock(foreground.mutex);
        if (worker) {
            --foreground.stuckWorkers;
        } else {
            foreground.mainThreadStuck = false;
        }
    }

    // Sleeps until something changed that may end the wait: the counter reached zero, or a job
    // the caller may run was queued. `withinDepth` says whether the caller may still run jobs
    // of its own lane.
    void sleepInWait(const JobCounter& counter, bool withinDepth) {
        sleepingWaiters.fetch_add(1);
        const std::uint32_t epoch = waitEpoch.load();
        // The lanes are read again after the sleep is announced, so a lane added meanwhile is seen.
        const HelpLanes help = helpLanes(counter.m_lanes.load(), withinDepth);
        if (counter.m_pending.load() != 0 && !anyQueued(help)) {
            // Foreground threads past the helping depth are what the watchdog counts.
            const bool worker = isForegroundWorker();
            const bool watched = !withinDepth && lane(JobLane::Foreground).workerCount > 0 &&
                                 (worker || (!isWorker() && std::this_thread::get_id() == mainThread));
            if (watched) {
                enterStuckSleep(counter, worker);
            }
            if (counter.m_pending.load() != 0) {
                waitEpoch.wait(epoch);
            }
            if (watched) {
                leaveStuckSleep(worker);
            }
        }
        sleepingWaiters.fetch_sub(1);
    }

    void wait(JobCounter& counter) {
        ITH_ASSERT_MSG(heldMutexCount() == 0, "JobSystem::wait called while holding an ith::Mutex");
        [[maybe_unused]] const std::uint8_t counterLanes = counter.m_lanes.load();
        ITH_ASSERT_MSG(
            !(g_thread.currentJob == JobLane::Foreground && (counterLanes & laneBit(JobLane::Background)) != 0),
            "A foreground job must not wait on background jobs: use a continuation or poll the counter");
        ITH_ASSERT_MSG(!(g_thread.currentJob.has_value() && (counterLanes & laneBit(JobLane::MainThread)) != 0),
                       "A job must not wait on main-thread jobs: use a continuation");

        const bool withinDepth = ++g_thread.waitDepth <= kMaxHelpingWaitDepth;
        while (!counter.isDone()) {
            if (Job* job = popHelpable(helpLanes(counter.m_lanes.load(), withinDepth))) {
                run(job);
                continue;
            }
            sleepInWait(counter, withinDepth);
        }
        --g_thread.waitDepth;
    }

    Job* acquire(JobLane jobLane) {
        if (jobLane != JobLane::Foreground) {
            return backgroundPool.tryAcquire();
        }
        Job* job = foregroundPool.tryAcquire();
        // Progress is any record changing hands anywhere, not only on this thread: a thread past
        // the helping depth, or one the OS didn't schedule for a while, must not mistake a busy
        // pool for one held by parked continuations.
        auto progressAt = std::chrono::steady_clock::now();
        std::uint64_t seenAcquisitions = foregroundPool.acquisitions();
        while (job == nullptr) {
            // Every foreground record is in use: run queued foreground jobs until one frees.
            // Past the helping depth only a lane without workers is still helped, as in wait().
            const Lane& foreground = lane(JobLane::Foreground);
            const bool mayRun = g_thread.waitDepth < kMaxHelpingWaitDepth || foreground.workerCount == 0;
            Job* queued = mayRun ? popFrom(JobLane::Foreground) : nullptr;
            if (queued != nullptr) {
                ITH_ASSERT_MSG(heldMutexCount() == 0,
                               "Foreground job pool exhausted while the submitting thread holds an ith::Mutex");
                ++g_thread.waitDepth;
                run(queued);
                --g_thread.waitDepth;
                progressAt = std::chrono::steady_clock::now();
            } else if (const std::uint64_t acquisitions = foregroundPool.acquisitions();
                       acquisitions != seenAcquisitions) {
                seenAcquisitions = acquisitions;
                progressAt = std::chrono::steady_clock::now();
                std::this_thread::yield();
            } else if (std::chrono::steady_clock::now() - progressAt >= kPoolExhaustedGrace) {
                // The records are held by parked continuations, or by queued jobs nobody runs: no
                // record will free (ADR-0026 rule 3).
                ITH_FATAL("Job system: foreground job pool exhausted by parked continuations (or by queued jobs "
                          "no thread can run). Park fewer continuations or raise "
                          "JobSystemConfig::foregroundJobCapacity.");
            } else {
                std::this_thread::yield();
            }
            job = foregroundPool.tryAcquire();
        }
        return job;
    }

    void workerMain(JobLane laneId, std::uint32_t index) {
        g_thread.workerOf = &system;
        g_thread.workerLane = laneId;
        g_thread.workerIndex = index;
        const bool foreground = laneId == JobLane::Foreground;
        const std::string name = std::string(foreground ? "Foreground " : "Background ") + std::to_string(index);
        detail::setCurrentThreadName(name.c_str());
        detail::setCurrentThreadPriority(foreground ? detail::ThreadPriority::Normal
                                                    : detail::ThreadPriority::BelowNormal);

        Lane& own = lane(laneId);
        for (;;) {
            // Spin a little before sleeping: waking a sleeping thread costs the submitter an OS
            // call and the job tens of microseconds.
            Job* job = own.pop(index);
            for (int spin = 1; job == nullptr && spin <= kIdleSpins; ++spin) {
                detail::cpuRelax();
                // Spinning thieves would fight over every new job, so they look at the other
                // queues only now and then.
                job = own.pop(index, spin % kStealInterval == 0);
            }
            if (job != nullptr) {
                run(job);
                continue;
            }
            const MutexLock lock(own.mutex);
            if (own.stopping && own.queued() == 0) {
                return;
            }
            // See Lane: the pusher sees this thread idle, or this thread sees the job. The pusher
            // takes the mutex before notifying, so the wake-up can't fall between check and wait.
            own.idleWorkers.fetch_add(1);
            if (own.queued() == 0 && !own.stopping) {
                own.wake.wait(own.mutex);
            }
            own.idleWorkers.fetch_sub(1);
        }
    }

    void start() {
        for (const JobLane laneId : {JobLane::Foreground, JobLane::Background}) {
            Lane& target = lane(laneId);
            target.workers.reserve(target.workerCount);
            for (std::uint32_t index = 0; index < target.workerCount; ++index) {
                target.workers.emplace_back([this, laneId, index] { workerMain(laneId, index); });
            }
        }
    }

    std::uint32_t runMainThreadJobs() {
        Lane& mainLane = lane(JobLane::MainThread);
        std::uint32_t count = 0;
        Job* jobs = mainLane.queues[0].takeAll(count);
        while (jobs != nullptr) {
            Job* next = jobs->next;
            run(jobs);
            jobs = next;
        }
        return count;
    }

    // True when no job is queued, parked or running: every record is free and every worker
    // sleeps. Called by the main thread during shutdown, when no other thread submits; a worker
    // only wakes for a new job, so once true it stays true.
    bool idle() {
        for (const JobLane laneId : {JobLane::Foreground, JobLane::Background}) {
            if (lane(laneId).idleWorkers.load() != lane(laneId).workerCount) {
                return false;
            }
        }
        for (const Lane& each : lanes) {
            if (each.queued() != 0) {
                return false;
            }
        }
        return foregroundPool.allFree() && backgroundPool.allFree();
    }

    // Runs or waits for every job, then stops and joins the workers.
    void shutdown() {
        shutdownRequested.store(true);
        for (int quietRounds = 0;;) {
            bool ran = runMainThreadJobs() > 0;
            for (const JobLane laneId : {JobLane::Foreground, JobLane::Background}) {
                if (Job* job = popFrom(laneId)) {
                    run(job);
                    ran = true;
                }
            }
            if (idle()) {
                break;
            }
            // Workers fall asleep within microseconds of running out of jobs; long background jobs
            // may take much longer, so the main thread stops spinning after a while.
            quietRounds = ran ? 0 : quietRounds + 1;
            if (quietRounds < kShutdownSpins) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        for (const JobLane laneId : {JobLane::Foreground, JobLane::Background}) {
            Lane& target = lane(laneId);
            {
                const MutexLock lock(target.mutex);
                target.stopping = true;
            }
            target.wake.notify_all();
            for (std::thread& worker : target.workers) {
                worker.join();
            }
        }
    }
};

JobSystem::JobSystem(const JobSystemConfig& config) {
    ITH_ASSERT_MSG(config.foregroundJobCapacity > 0, "JobSystemConfig::foregroundJobCapacity must be at least 1");
    m_state = std::make_unique<State>(*this, config);
    m_state->start();
}

JobSystem::~JobSystem() {
    ITH_ASSERT_MSG(isMainThread(), "A JobSystem must be destroyed on the thread that created it");
    ITH_ASSERT_MSG(!g_thread.currentJob.has_value(), "A JobSystem must not be destroyed inside a job");
    m_state->shutdown();
}

void JobSystem::wait(JobCounter& counter) {
    m_state->wait(counter);
}

std::uint32_t JobSystem::runMainThreadJobs() {
    ITH_ASSERT_MSG(isMainThread(), "runMainThreadJobs must be called on the main thread");
    ITH_ASSERT_MSG(!g_thread.currentJob.has_value(), "runMainThreadJobs must not be called inside a job");
    return m_state->runMainThreadJobs();
}

std::uint32_t JobSystem::workerCount(JobLane lane) const {
    return m_state->lanes[laneIndex(lane)].workerCount;
}

bool JobSystem::isMainThread() const {
    return std::this_thread::get_id() == m_state->mainThread;
}

bool JobSystem::shutdownRequested() const {
    return m_state->shutdownRequested.load();
}

detail::Job* JobSystem::acquireJob(JobLane lane, const char* name, JobCounter* counter) {
    Job* job = m_state->acquire(lane);
    job->name = name;
    job->lane = lane;
    job->counter = counter;
    if (counter != nullptr) {
        [[maybe_unused]] const std::uint32_t previous = counter->m_pending.fetch_add(1);
        ITH_ASSERT_MSG(previous != std::numeric_limits<std::uint32_t>::max(), "JobCounter overflow");
        if ((counter->m_lanes.load(std::memory_order_relaxed) & laneBit(lane)) == 0) {
            counter->m_lanes.fetch_or(laneBit(lane));
        }
    }
    return job;
}

void JobSystem::enqueue(detail::Job* job) {
    m_state->enqueue(job);
}

void JobSystem::enqueueAfter(JobCounter& after, detail::Job* job) {
    // A job counted by the counter it waits for would never run: the counter couldn't reach zero.
    // Should the assert continue, the job is queued at once instead.
    ITH_ASSERT_MSG(&after != job->counter, "submitAfter: a continuation can't be counted by the counter it waits for");
    if (&after == job->counter) {
        m_state->enqueue(job);
        return;
    }
    {
        const MutexLock lock(after.m_mutex);
        if (after.m_pending.load() != 0) {
            job->next = after.m_continuations;
            after.m_continuations = job;
            return;
        }
    }
    m_state->enqueue(job);
}

void JobSystem::runParallelFor(const char* name, ParallelFor& work) {
    ITH_ASSERT_MSG(work.batchSize > 0, "parallelFor needs a batch size of at least 1");
    if (work.count == 0) {
        return;
    }
    ITH_ASSERT_MSG(work.count <= std::numeric_limits<std::size_t>::max() / 2, "parallelFor count too large");

    struct Shared {
        ParallelFor* work = nullptr;
        std::atomic<std::size_t> cursor{0};

        // Claims and runs ranges until none is left.
        void runRanges() {
            for (;;) {
                const std::size_t begin = cursor.fetch_add(work->batchSize);
                if (begin >= work->count) {
                    return;
                }
                const std::size_t end = std::min(begin + work->batchSize, work->count);
                work->call(work->body, begin, end);
            }
        }
    };

    Shared shared;
    shared.work = &work;

    const std::size_t ranges = work.count / work.batchSize + (work.count % work.batchSize != 0 ? 1 : 0);
    const std::size_t helpers = std::min<std::size_t>(workerCount(JobLane::Foreground), ranges - 1);
    JobCounter counter;
    for (std::size_t index = 0; index < helpers; ++index) {
        submit(JobLane::Foreground, name, [&shared] { shared.runRanges(); }, &counter);
    }
    shared.runRanges();
    wait(counter);
}

} // namespace ith
