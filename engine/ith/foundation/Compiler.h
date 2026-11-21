#pragma once

// Compiler, sanitizer and thread-safety hooks that expand to nothing where they are not supported.
//
//   ITH_LIFETIMEBOUND            marks a parameter whose referent the return value refers to,
//                                so compilers can warn when it would dangle:
//                                  std::string_view trim(std::string_view text ITH_LIFETIMEBOUND);
//   ITH_ASAN_POISON(ptr, size)   in AddressSanitizer builds, marks memory as not addressable, so
//   ITH_ASAN_UNPOISON(ptr, size) a custom allocator's free memory is reported when touched.

#ifdef __has_cpp_attribute
#if __has_cpp_attribute(clang::lifetimebound)
#define ITH_LIFETIMEBOUND [[clang::lifetimebound]]
#elif __has_cpp_attribute(msvc::lifetimebound)
#define ITH_LIFETIMEBOUND [[msvc::lifetimebound]]
#endif
#endif
#ifndef ITH_LIFETIMEBOUND
#define ITH_LIFETIMEBOUND
#endif

#ifdef __SANITIZE_ADDRESS__
#define ITH_HAS_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ITH_HAS_ASAN 1
#endif
#endif

#ifdef ITH_HAS_ASAN
#include <sanitizer/asan_interface.h>
#define ITH_ASAN_POISON(pointer, size) __asan_poison_memory_region((pointer), (size))
#define ITH_ASAN_UNPOISON(pointer, size) __asan_unpoison_memory_region((pointer), (size))
#else
#define ITH_ASAN_POISON(pointer, size) static_cast<void>(sizeof(pointer) + sizeof(size))
#define ITH_ASAN_UNPOISON(pointer, size) static_cast<void>(sizeof(pointer) + sizeof(size))
#endif

// Clang thread-safety analysis (-Wthread-safety, ADR-0001). Shared state names the mutex that
// guards it, and functions name the mutexes they take or need; other compilers ignore them.
//
//   ith::Mutex m_mutex;
//   int m_count ITH_GUARDED_BY(m_mutex) = 0;
//   void addLocked() ITH_REQUIRES(m_mutex);
//
//   ITH_CAPABILITY(name)      a type that is a lock (ith::Mutex)
//   ITH_SCOPED_CAPABILITY     a type that holds a lock for its lifetime (ith::MutexLock)
//   ITH_GUARDED_BY(mutex)     a member that may be accessed only while `mutex` is held
//   ITH_PT_GUARDED_BY(mutex)  a pointer member whose pointee may be accessed only while `mutex` is held
//   ITH_REQUIRES(mutex)       a function that may be called only while `mutex` is held
//   ITH_EXCLUDES(mutex)       a function that must not be called while `mutex` is held
//   ITH_ACQUIRE(...)          a function that takes the lock (no argument: the object itself)
//   ITH_RELEASE(...)          a function that releases the lock
//   ITH_NO_THREAD_SAFETY_ANALYSIS  turns the analysis off for one function; say why next to it
#ifdef __clang__
#define ITH_THREAD_ANNOTATION(attribute) __attribute__((attribute))
#else
#define ITH_THREAD_ANNOTATION(attribute)
#endif
#define ITH_CAPABILITY(name) ITH_THREAD_ANNOTATION(capability(name))
#define ITH_SCOPED_CAPABILITY ITH_THREAD_ANNOTATION(scoped_lockable)
#define ITH_GUARDED_BY(mutex) ITH_THREAD_ANNOTATION(guarded_by(mutex))
#define ITH_PT_GUARDED_BY(mutex) ITH_THREAD_ANNOTATION(pt_guarded_by(mutex))
#define ITH_REQUIRES(...) ITH_THREAD_ANNOTATION(requires_capability(__VA_ARGS__))
#define ITH_EXCLUDES(...) ITH_THREAD_ANNOTATION(locks_excluded(__VA_ARGS__))
#define ITH_ACQUIRE(...) ITH_THREAD_ANNOTATION(acquire_capability(__VA_ARGS__))
#define ITH_RELEASE(...) ITH_THREAD_ANNOTATION(release_capability(__VA_ARGS__))
#define ITH_NO_THREAD_SAFETY_ANALYSIS ITH_THREAD_ANNOTATION(no_thread_safety_analysis)
