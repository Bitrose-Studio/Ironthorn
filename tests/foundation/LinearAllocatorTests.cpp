#include "ith/foundation/LinearAllocator.h"

#include "AssertCapture.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

struct Point {
    float x = 0.0f;
    float y = 0.0f;
};

// Counts its constructions, to check that a failed create constructs nothing.
struct Counted {
    explicit Counted(int& constructions) { ++constructions; }
};

// Fills its alignment exactly: MSVC warns about padding added for an alignment specifier.
struct alignas(64) CacheLine {
    std::array<std::uint8_t, 64> bytes{};
};

bool isAligned(const void* pointer, std::size_t alignment) {
    return reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0;
}

} // namespace

TEST_SUITE("foundation.linear_allocator") {
    TEST_CASE("a new arena is empty") {
        const ith::LinearAllocator arena(256);
        CHECK(arena.capacity() == 256);
        CHECK(arena.used() == 0);
        CHECK(arena.peak() == 0);
        CHECK(arena.mark() == 0);
    }

    TEST_CASE("allocations are aligned, distinct and inside the arena") {
        ith::LinearAllocator arena(1024);
        auto* first = static_cast<std::byte*>(arena.allocate(3, 1));
        auto* second = static_cast<std::byte*>(arena.allocate(8, 8));
        auto* third = static_cast<std::byte*>(arena.allocate(16, 64));
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);
        REQUIRE(third != nullptr);
        CHECK(isAligned(second, 8));
        CHECK(isAligned(third, 64));
        CHECK(second >= first + 3);
        CHECK(third >= second + 8);
        // The first allocation (alignment 1) starts the arena.
        CHECK(third + 16 <= first + arena.capacity());
        CHECK(arena.used() >= 3 + 8 + 16);
    }

    TEST_CASE("alignments stricter than the buffer's own are honoured") {
        ith::LinearAllocator arena(4096);
        static_cast<void>(arena.allocate(1, 1));
        void* page = arena.allocate(16, 1024);
        REQUIRE(page != nullptr);
        CHECK(isAligned(page, 1024));
    }

    TEST_CASE("an alignment larger than the arena fails without using space") {
        ith::LinearAllocator arena(64);
        REQUIRE(arena.allocate(1, 1) != nullptr);
        CHECK(arena.allocate(1, std::size_t{1} << 20) == nullptr);
        CHECK(arena.used() == 1);
    }

    TEST_CASE("the arena fills exactly to its capacity and then fails") {
        ith::LinearAllocator arena(64);
        CHECK(arena.allocate(64, 1) != nullptr);
        CHECK(arena.used() == 64);
        CHECK(arena.allocate(1, 1) == nullptr);
        // A zero-byte allocation uses no space, whatever address it returns.
        static_cast<void>(arena.allocate(0, 1));
        CHECK(arena.used() == 64);
    }

    TEST_CASE("a failed allocation changes nothing") {
        ith::LinearAllocator arena(64);
        REQUIRE(arena.allocate(10, 1) != nullptr);
        CHECK(arena.allocate(100, 1) == nullptr);
        CHECK(arena.allocate(static_cast<std::size_t>(-1), 1) == nullptr);
        CHECK(arena.used() == 10);
        CHECK(arena.allocate(54, 1) != nullptr);
    }

    TEST_CASE("a zero-capacity arena fails every non-empty allocation") {
        ith::LinearAllocator arena(0);
        CHECK(arena.capacity() == 0);
        CHECK(arena.allocate(1, 1) == nullptr);
        CHECK(arena.allocateArray<int>(1).empty());
        CHECK(arena.create<int>(1) == nullptr);
    }

    TEST_CASE("create constructs objects in place") {
        ith::LinearAllocator arena(256);
        Point* point = arena.create<Point>(Point{.x = 1.0f, .y = 2.0f});
        REQUIRE(point != nullptr);
        CHECK(point->x == 1.0f);
        CHECK(point->y == 2.0f);
        CHECK(isAligned(point, alignof(Point)));
    }

    TEST_CASE("create on a full arena returns nullptr and constructs nothing") {
        ith::LinearAllocator arena(sizeof(Counted));
        int constructions = 0;
        REQUIRE(arena.create<Counted>(constructions) != nullptr);
        CHECK(constructions == 1);
        CHECK(arena.create<Counted>(constructions) == nullptr);
        CHECK(constructions == 1);
    }

    TEST_CASE("allocateArray value-initializes its elements") {
        ith::LinearAllocator arena(1024);
        // Dirty the memory first, so the zeroes below come from value-initialization.
        const std::span<std::uint32_t> dirty = arena.allocateArray<std::uint32_t>(16);
        REQUIRE(dirty.size() == 16);
        for (std::uint32_t& value : dirty) {
            value = 0xdeadbeefu;
        }
        arena.reset();

        const std::span<std::uint32_t> values = arena.allocateArray<std::uint32_t>(16);
        REQUIRE(values.size() == 16);
        for (const std::uint32_t value : values) {
            CHECK(value == 0);
        }
    }

    TEST_CASE("allocateArray aligns over-aligned element types") {
        ith::LinearAllocator arena(1024);
        static_cast<void>(arena.allocate(1, 1));
        const std::span<CacheLine> lines = arena.allocateArray<CacheLine>(4);
        REQUIRE(lines.size() == 4);
        CHECK(isAligned(lines.data(), 64));
        CHECK(lines[3].bytes[63] == 0);
    }

    TEST_CASE("allocateArray returns an empty span for zero, too many and overflowing counts") {
        ith::LinearAllocator arena(64);
        CHECK(arena.allocateArray<std::uint64_t>(0).empty());
        CHECK(arena.allocateArray<std::uint64_t>(9).empty());
        CHECK(arena.allocateArray<std::uint64_t>(static_cast<std::size_t>(-1)).empty());
        CHECK(arena.used() == 0);
        CHECK(arena.allocateArray<std::uint64_t>(8).size() == 8);
    }

    TEST_CASE("rewind releases everything after a marker") {
        ith::LinearAllocator arena(256);
        REQUIRE(arena.allocate(16, 1) != nullptr);
        const ith::LinearAllocator::Marker marker = arena.mark();
        void* scratch = arena.allocate(100, 1);
        REQUIRE(scratch != nullptr);
        arena.rewind(marker);
        CHECK(arena.used() == 16);
        // The released space is handed out again.
        CHECK(arena.allocate(100, 1) == scratch);
    }

    TEST_CASE("reset releases everything but keeps the peak") {
        ith::LinearAllocator arena(256);
        REQUIRE(arena.allocate(200, 1) != nullptr);
        arena.reset();
        CHECK(arena.used() == 0);
        CHECK(arena.peak() == 200);
        REQUIRE(arena.allocate(50, 1) != nullptr);
        CHECK(arena.peak() == 200);
    }

#ifdef ITH_ENABLE_ASSERTS
    TEST_CASE("an alignment that is not a power of two asserts and fails") {
        ith::LinearAllocator arena(256);
        ith::test::AssertCapture capture;
        CHECK(arena.allocate(8, 3) == nullptr);
        CHECK(arena.allocate(8, 0) == nullptr);
        CHECK(capture.count() == 2);
        CHECK(arena.used() == 0);
    }

    TEST_CASE("rewinding to a marker ahead of the current position asserts and is ignored") {
        ith::LinearAllocator arena(256);
        REQUIRE(arena.allocate(64, 1) != nullptr);
        const ith::LinearAllocator::Marker stale = arena.mark();
        arena.reset();
        REQUIRE(arena.allocate(8, 1) != nullptr);

        ith::test::AssertCapture capture;
        arena.rewind(stale);
        CHECK(capture.count() == 1);
        CHECK(arena.used() == 8);
    }
#endif
}
