// The process-wide parts of the ECS under concurrent use: world ids and the component registry.
// Each thread uses only its own worlds, as the World's contract requires. Most useful under
// ThreadSanitizer.

#include "ith/ecs/World.h"
#include "ith/foundation/Hash.h"

#include "EcsComponents.h"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <set>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr int kThreads = 4;
constexpr int kTypesPerThread = 2;

constexpr std::array<std::string_view, kThreads * kTypesPerThread> kNames = {
    "ecs_test::Threaded0", "ecs_test::Threaded1", "ecs_test::Threaded2", "ecs_test::Threaded3",
    "ecs_test::Threaded4", "ecs_test::Threaded5", "ecs_test::Threaded6", "ecs_test::Threaded7",
};

// Distinct component types, registered for the first time by different threads.
template <int N>
struct Threaded {
    ITH_TYPE(Component)

    int value = N;
};

template <int N>
const ith::TypeInfo& Threaded<N>::ithTypeInfo() noexcept {
    static constexpr ith::TypeInfo kInfo = ith::detail::makeStructType<Threaded<N>>(
        ith::fnv1a64(kNames[N]), kNames[N], ith::TypeRole::Component, ith::ReflectionFlags::None, {}, {});
    return kInfo;
}

template <int First>
void work(int rounds, std::atomic<bool>& start, std::atomic<int>& failures) {
    while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    for (int round = 0; round < rounds; ++round) {
        ith::World world;
        const ith::Entity entity =
            world.spawn(Threaded<First>{}, Threaded<First + 1>{}, ith::test::Health{.value = round});
        int sum = 0;
        world.query<const Threaded<First>, const Threaded<First + 1>>().forEach(
            [&](const Threaded<First>& a, const Threaded<First + 1>& b) { sum += a.value + b.value; });
        if (sum != (2 * First) + 1 || world.get<ith::test::Health>(entity)->value != round) {
            failures.fetch_add(1);
        }
    }
}

} // namespace

TEST_CASE("World: worlds and component types can be created from several threads at once") {
    std::atomic<bool> start{false};
    std::atomic<int> failures{0};
    constexpr int kRounds = 200;
    std::vector<std::thread> threads;
    threads.emplace_back([&] { work<0>(kRounds, start, failures); });
    threads.emplace_back([&] { work<2>(kRounds, start, failures); });
    threads.emplace_back([&] { work<4>(kRounds, start, failures); });
    threads.emplace_back([&] { work<6>(kRounds, start, failures); });
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(failures.load() == 0);

    // Every type got its own index, and a later world sees the same ones.
    std::set<ith::detail::ComponentIndex> indices = {
        ith::detail::componentIndexOf<Threaded<0>>(), ith::detail::componentIndexOf<Threaded<1>>(),
        ith::detail::componentIndexOf<Threaded<2>>(), ith::detail::componentIndexOf<Threaded<3>>(),
        ith::detail::componentIndexOf<Threaded<4>>(), ith::detail::componentIndexOf<Threaded<5>>(),
        ith::detail::componentIndexOf<Threaded<6>>(), ith::detail::componentIndexOf<Threaded<7>>(),
    };
    CHECK(indices.size() == kNames.size());
    static_assert(kThreads * kTypesPerThread == 8);
}
