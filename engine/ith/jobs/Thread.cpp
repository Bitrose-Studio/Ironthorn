#include "ith/jobs/Thread.h"

// The job system owns its threads (ADR-0025 rule 6), so the native calls for them live here,
// one branch per platform.
#ifdef _WIN32
#include <windows.h>

#include <string>
#elif defined(__linux__)
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#elif defined(__APPLE__)
#include <pthread.h>
#endif

namespace ith::detail {

void setCurrentThreadName(const char* name) {
#ifdef _WIN32
    // Worker names are ASCII, so widening byte by byte is enough.
    std::wstring wide;
    for (const char* character = name; *character != '\0'; ++character) {
        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*character)));
    }
    static_cast<void>(SetThreadDescription(GetCurrentThread(), wide.c_str()));
#elif defined(__linux__)
    // Linux keeps 15 bytes and fails on longer names.
    std::array<char, 16> shortName{};
    std::memcpy(shortName.data(), name, std::min(std::strlen(name), shortName.size() - 1));
    static_cast<void>(pthread_setname_np(pthread_self(), shortName.data()));
#elif defined(__APPLE__)
    static_cast<void>(pthread_setname_np(name));
#else
    static_cast<void>(name);
#endif
}

void setCurrentThreadPriority(ThreadPriority priority) {
#ifdef _WIN32
    const int value = priority == ThreadPriority::BelowNormal ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL;
    static_cast<void>(SetThreadPriority(GetCurrentThread(), value));
#elif defined(__linux__)
    // On Linux the nice value belongs to the thread (its kernel task), so this lowers only the
    // calling thread. Any process may raise its own nice value; lowering it back needs privileges,
    // which is why Normal leaves the thread as it is.
    if (priority == ThreadPriority::BelowNormal) {
        const auto threadId = static_cast<id_t>(syscall(SYS_gettid));
        static_cast<void>(setpriority(PRIO_PROCESS, threadId, getpriority(PRIO_PROCESS, threadId) + 5));
    }
#else
    static_cast<void>(priority);
#endif
}

} // namespace ith::detail
