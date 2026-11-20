#include "ith/foundation/LinearAllocator.h"

#include "ith/foundation/Assert.h"
#include "ith/foundation/Compiler.h"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace ith {

// The array form leaves the bytes uninitialized; see m_buffer.
LinearAllocator::LinearAllocator(std::size_t capacity)
    : m_buffer(std::make_unique_for_overwrite<std::byte[]>(capacity)), // NOLINT(modernize-avoid-c-arrays)
      m_capacity(capacity) {
    ITH_ASAN_POISON(m_buffer.get(), m_capacity);
}

LinearAllocator::~LinearAllocator() {
    // Hand the memory back to the system allocator in the state it gave it out.
    ITH_ASAN_UNPOISON(m_buffer.get(), m_capacity);
}

void* LinearAllocator::allocate(std::size_t size, std::size_t alignment) {
    ITH_ASSERT_MSG(std::has_single_bit(alignment), "LinearAllocator: the alignment must be a power of two");
    if (!std::has_single_bit(alignment)) {
        return nullptr;
    }

    // Align the address, not the offset, so alignments stricter than the buffer's own are
    // honoured.
    const auto address = reinterpret_cast<std::uintptr_t>(m_buffer.get()) + m_offset;
    const auto padding = static_cast<std::size_t>((alignment - (address & (alignment - 1))) & (alignment - 1));
    const std::size_t remaining = m_capacity - m_offset;
    if (padding > remaining || size > remaining - padding) {
        return nullptr;
    }

    const std::size_t start = m_offset + padding;
    ITH_ASAN_UNPOISON(m_buffer.get() + start, size);
    m_offset = start + size;
    m_peak = std::max(m_peak, m_offset);
    return m_buffer.get() + start;
}

void LinearAllocator::rewind(Marker marker) {
    ITH_ASSERT_MSG(marker <= m_offset, "LinearAllocator: the marker is ahead of the current position");
    if (marker > m_offset) {
        return;
    }
    ITH_ASAN_POISON(m_buffer.get() + marker, m_offset - marker);
    m_offset = marker;
}

} // namespace ith
