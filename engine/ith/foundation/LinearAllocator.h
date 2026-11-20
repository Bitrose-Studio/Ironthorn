#pragma once

// A linear ("arena" or "bump") allocator for short-lived data.
//
// Allocating moves an offset forward; single allocations are never freed. The whole arena is
// reset at once, or rewound to a marker taken earlier. This suits per-frame scratch data that
// is rebuilt every frame.
//
//   ith::LinearAllocator frame(4 * 1024 * 1024);
//   const std::span<DrawItem> items = frame.allocateArray<DrawItem>(visibleCount);
//   if (items.empty() && visibleCount > 0) { /* the arena is too small */ }
//   ...
//   frame.reset(); // at the end of the frame
//
// Ownership: the arena owns its memory. Pointers and spans it returns are non-owning views that
// stay valid until reset(), a rewind() to before them, or the arena's destruction. Destructors
// are never run, so only trivially destructible types can be created in it. In AddressSanitizer
// builds, memory that is not allocated is poisoned, so a stale pointer is reported when used.
//
// Not thread-safe: use one arena per thread, or synchronize access.

#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace ith {

class LinearAllocator {
public:
    // A position in the arena, taken with mark() and passed to rewind().
    using Marker = std::size_t;

    // Reserves `capacity` bytes up front, without initializing them; the arena never grows.
    // Running out of system memory here ends the process, as for any allocation in a build
    // without exceptions.
    explicit LinearAllocator(std::size_t capacity);

    LinearAllocator(const LinearAllocator&) = delete;
    LinearAllocator& operator=(const LinearAllocator&) = delete;
    LinearAllocator(LinearAllocator&&) = delete;
    LinearAllocator& operator=(LinearAllocator&&) = delete;
    ~LinearAllocator();

    // Returns `size` uninitialized bytes aligned to `alignment`, or nullptr when the arena
    // doesn't have enough space left. A zero `size` returns some address or nullptr; don't
    // dereference it. Precondition (asserted): `alignment` is a power of two; otherwise returns nullptr.
    [[nodiscard]] void* allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t));

    // Constructs a T in the arena from `args` and returns it, or nullptr when the arena is full.
    template <typename T, typename... Args>
        requires std::is_trivially_destructible_v<T>
    [[nodiscard]] T* create(Args&&... args) {
        void* memory = allocate(sizeof(T), alignof(T));
        return memory != nullptr ? ::new (memory) T(std::forward<Args>(args)...) : nullptr;
    }

    // Allocates `count` value-initialized elements. Returns an empty span when `count` is zero
    // or the arena doesn't have enough space left.
    template <typename T>
        requires std::is_trivially_destructible_v<T>
    [[nodiscard]] std::span<T> allocateArray(std::size_t count) {
        if (count == 0 || count > capacity() / sizeof(T)) {
            return {};
        }
        auto* elements = static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
        if (elements == nullptr) {
            return {};
        }
        // Not placement array-new: that form may add a size cookie before the elements.
        std::uninitialized_value_construct_n(elements, count);
        return {elements, count};
    }

    // The current position, for a later rewind().
    [[nodiscard]] Marker mark() const { return m_offset; }

    // Releases everything allocated after `marker` was taken. Precondition (asserted): `marker`
    // is not ahead of the current position, i.e. it was not invalidated by an earlier rewind or
    // reset; such a marker is ignored.
    void rewind(Marker marker);

    // Releases every allocation. peak() is kept.
    void reset() { rewind(0); }

    // The number of bytes reserved at construction.
    [[nodiscard]] std::size_t capacity() const { return m_capacity; }

    // The number of bytes in use, including alignment padding.
    [[nodiscard]] std::size_t used() const { return m_offset; }

    // The largest value used() has reached, for sizing the arena.
    [[nodiscard]] std::size_t peak() const { return m_peak; }

private:
    // A C array, so the bytes are not zero-filled: make_unique_for_overwrite leaves them
    // uninitialized.
    std::unique_ptr<std::byte[]> m_buffer; // NOLINT(modernize-avoid-c-arrays)
    std::size_t m_capacity = 0;
    std::size_t m_offset = 0;
    std::size_t m_peak = 0;
};

} // namespace ith
