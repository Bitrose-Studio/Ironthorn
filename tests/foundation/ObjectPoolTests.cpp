#include "ith/foundation/ObjectPool.h"

#include "AssertCapture.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

// Counts constructions and destructions, to check that the pool runs both.
struct Tracked {
    static inline int s_alive = 0;

    explicit Tracked(int initial) : value(initial) { ++s_alive; }

    ~Tracked() { --s_alive; }

    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
    Tracked(Tracked&&) = delete;
    Tracked& operator=(Tracked&&) = delete;

    int value;
};

// Fills its alignment exactly: MSVC warns about padding added for an alignment specifier.
struct alignas(64) OverAligned {
    std::array<std::uint8_t, 64> bytes{};
};

// Overloads unary operator&, so the pool must use std::addressof.
struct AddressHidden {
    int value = 0;

    void operator&() const = delete;
};

bool isAligned(const void* pointer, std::size_t alignment) {
    return reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0;
}

} // namespace

TEST_SUITE("foundation.object_pool") {
    TEST_CASE("a new pool holds nothing and has allocated nothing") {
        const ith::ObjectPool<int> pool;
        CHECK(pool.liveCount() == 0);
        CHECK(pool.capacity() == 0);
    }

    TEST_CASE("create constructs an object and the Ptr destroys it") {
        Tracked::s_alive = 0;
        ith::ObjectPool<Tracked> pool(4);
        {
            const ith::ObjectPool<Tracked>::Ptr object = pool.create(7);
            REQUIRE(object != nullptr);
            CHECK(object->value == 7);
            CHECK(Tracked::s_alive == 1);
            CHECK(pool.liveCount() == 1);
            CHECK(pool.capacity() == 4);
        }
        CHECK(Tracked::s_alive == 0);
        CHECK(pool.liveCount() == 0);
    }

    TEST_CASE("objects work with strings and other non-trivial types") {
        ith::ObjectPool<std::string> pool(2);
        const auto a = pool.create("a long string that does not fit in the small-string buffer");
        const auto b = pool.create(3, 'x');
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        CHECK(*a == "a long string that does not fit in the small-string buffer");
        CHECK(*b == "xxx");
    }

    TEST_CASE("the pool grows by whole chunks") {
        ith::ObjectPool<int> pool(3);
        std::vector<ith::ObjectPool<int>::Ptr> objects;
        for (int index = 0; index < 7; ++index) {
            objects.push_back(pool.create(index));
            REQUIRE(objects.back() != nullptr);
        }
        CHECK(pool.liveCount() == 7);
        CHECK(pool.capacity() == 9);

        std::set<const int*> addresses;
        for (int index = 0; index < 7; ++index) {
            CHECK(*objects[static_cast<std::size_t>(index)] == index);
            addresses.insert(objects[static_cast<std::size_t>(index)].get());
        }
        CHECK(addresses.size() == 7);
    }

    TEST_CASE("a freed slot is reused before the pool grows") {
        ith::ObjectPool<int> pool(2);
        auto first = pool.create(1);
        auto second = pool.create(2);
        const int* firstAddress = first.get();
        first.reset();
        CHECK(pool.liveCount() == 1);

        const auto third = pool.create(3);
        CHECK(third.get() == firstAddress);
        CHECK(pool.capacity() == 2);
        CHECK(*second == 2);
    }

    TEST_CASE("object types smaller than a pointer are supported") {
        ith::ObjectPool<std::uint8_t> pool(16);
        std::vector<ith::ObjectPool<std::uint8_t>::Ptr> objects;
        for (int index = 0; index < 32; ++index) {
            objects.push_back(pool.create(static_cast<std::uint8_t>(index)));
        }
        for (int index = 0; index < 32; ++index) {
            CHECK(*objects[static_cast<std::size_t>(index)] == index);
        }
    }

    TEST_CASE("over-aligned objects are aligned") {
        ith::ObjectPool<OverAligned> pool(3);
        std::vector<ith::ObjectPool<OverAligned>::Ptr> objects;
        for (int index = 0; index < 5; ++index) {
            objects.push_back(pool.create());
            REQUIRE(objects.back() != nullptr);
            CHECK(isAligned(objects.back().get(), 64));
        }
    }

    TEST_CASE("a Ptr can be moved and released explicitly") {
        ith::ObjectPool<int> pool;
        auto object = pool.create(5);
        ith::ObjectPool<int>::Ptr moved = std::move(object);
        CHECK(object == nullptr);
        REQUIRE(moved != nullptr);
        CHECK(pool.liveCount() == 1);
        moved = nullptr;
        CHECK(pool.liveCount() == 0);
    }

    TEST_CASE("move-assigning a Ptr from another pool destroys the old object in its own pool") {
        Tracked::s_alive = 0;
        ith::ObjectPool<Tracked> first;
        ith::ObjectPool<Tracked> second;
        ith::ObjectPool<Tracked>::Ptr target = first.create(1);
        ith::ObjectPool<Tracked>::Ptr source = second.create(2);

        target = std::move(source);
        CHECK(first.liveCount() == 0);
        CHECK(second.liveCount() == 1);
        CHECK(Tracked::s_alive == 1);
        REQUIRE(target != nullptr);
        CHECK(target->value == 2);

        // The moved deleter returns the object to the pool that created it.
        target.reset();
        CHECK(second.liveCount() == 0);
        CHECK(Tracked::s_alive == 0);
    }

    TEST_CASE("a reused slot holds a fresh std::string") {
        ith::ObjectPool<std::string> pool(1);
        auto first = pool.create("a long string that does not fit in the small-string buffer");
        const std::string* address = first.get();
        first.reset();

        const auto second = pool.create("short");
        CHECK(second.get() == address);
        CHECK(*second == "short");
        CHECK(pool.capacity() == 1);
    }

    TEST_CASE("types that overload operator& are supported") {
        ith::ObjectPool<AddressHidden> pool;
        const auto object = pool.create(AddressHidden{.value = 4});
        REQUIRE(object != nullptr);
        CHECK(object->value == 4);
    }

    TEST_CASE("a default-constructed Ptr is empty and safe to destroy") {
        const ith::ObjectPool<int>::Ptr empty;
        CHECK(empty == nullptr);
    }

#ifdef ITH_ENABLE_ASSERTS
    TEST_CASE("a zero chunk size asserts and falls back to one object per chunk") {
        ith::test::AssertCapture capture;
        ith::ObjectPool<int> pool(0);
        CHECK(capture.count() == 1);
        const auto object = pool.create(1);
        REQUIRE(object != nullptr);
        CHECK(pool.capacity() == 1);
    }

    TEST_CASE("a Deleter without a pool asserts and leaves the object alone") {
        ith::test::AssertCapture capture;
        int local = 3;
        const ith::ObjectPool<int>::Deleter deleter;
        deleter(&local);
        CHECK(capture.count() == 1);
        CHECK(local == 3);
        // A null object is fine.
        deleter(nullptr);
        CHECK(capture.count() == 1);
    }
#endif
}
