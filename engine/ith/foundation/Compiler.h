#pragma once

// Compiler and sanitizer hooks that expand to nothing where they are not supported.
//
//   ITH_LIFETIMEBOUND            marks a parameter whose referent the return value refers to,
//                                so compilers can warn when it would dangle:
//                                  std::string_view trim(std::string_view text ITH_LIFETIMEBOUND);
//   ITH_ASAN_POISON(ptr, size)   in AddressSanitizer builds, marks memory as not addressable, so
//   ITH_ASAN_UNPOISON(ptr, size) a custom allocator's free memory is reported when touched.

#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::lifetimebound)
#define ITH_LIFETIMEBOUND [[clang::lifetimebound]]
#elif __has_cpp_attribute(msvc::lifetimebound)
#define ITH_LIFETIMEBOUND [[msvc::lifetimebound]]
#endif
#endif
#ifndef ITH_LIFETIMEBOUND
#define ITH_LIFETIMEBOUND
#endif

#if defined(__SANITIZE_ADDRESS__)
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
