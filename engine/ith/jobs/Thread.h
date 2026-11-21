#pragma once

// The job system's thread layer (ADR-0025 rule 6): names and priorities of worker threads, and
// the spin hint. A console port adds its own branch here and in Thread.cpp. Internal to the jobs
// module.

#include <cstdint>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#elif defined(_M_ARM64)
#include <intrin.h>
#endif

namespace ith::detail {

enum class ThreadPriority : std::uint8_t {
    Normal,
    BelowNormal, // Preempted by the OS in favour of normal-priority threads.
};

// Names the calling thread for debuggers, profilers and crash reports. Long names are cut to
// what the platform keeps (15 bytes on Linux). Failures are ignored.
void setCurrentThreadName(const char* name);

// Sets the OS priority of the calling thread. Failures are ignored: a thread that keeps its
// priority still works.
void setCurrentThreadPriority(ThreadPriority priority);

// Tells the CPU the calling thread is spinning, so it saves power and yields resources to the
// other hardware thread of its core. A few dozen cycles; not an OS call.
inline void cpuRelax() {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(_M_ARM64)
    __yield();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#endif
}

} // namespace ith::detail
