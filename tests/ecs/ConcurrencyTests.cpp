// The ECS under concurrent use: world ids and the component registry from several threads, worlds
// built by type record in parallel jobs, and a world handed from job to job. Each world is used by
// one thread at a time, as the World's contract requires. Most useful under ThreadSanitizer.

#include "ith/ecs/World.h"
#include "ith/foundation/Hash.h"
#include "ith/jobs/JobSystem.h"
#include "ith/reflection/TypeOf.h"

#include "EcsComponents.h"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
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

namespace {

constexpr std::array<std::string_view, 4> kRecordNames = {
    "ecs_test::ByRecord0",
    "ecs_test::ByRecord1",
    "ecs_test::ByRecord2",
    "ecs_test::ByRecord3",
};

// Component types reached only through their records, registered for the first time by
// different jobs.
template <int N>
struct ByRecord {
    ITH_TYPE(Component)

    int value = N;
};

template <int N>
const ith::TypeInfo& ByRecord<N>::ithTypeInfo() noexcept {
    static constexpr ith::TypeInfo kInfo = ith::detail::makeStructType<ByRecord<N>>(
        ith::fnv1a64(kRecordNames[N]), kRecordNames[N], ith::TypeRole::Component, ith::ReflectionFlags::None, {}, {});
    return kInfo;
}

} // namespace

TEST_CASE("World: jobs build worlds by type record, with resources, in parallel") {
    const std::array<const ith::TypeInfo*, kRecordNames.size()> records = {
        &ith::typeOf<ByRecord<0>>(), &ith::typeOf<ByRecord<1>>(), &ith::typeOf<ByRecord<2>>(),
        &ith::typeOf<ByRecord<3>>()};
    std::atomic<int> failures{0};
    ith::JobSystem jobs(ith::JobSystemConfig{.foregroundWorkers = 4, .backgroundWorkers = 0});
    constexpr std::size_t kWorlds = 64;
    jobs.parallelFor("Worlds", kWorlds, 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t index = begin; index < end; ++index) {
            ith::World world;
            const ith::TypeInfo& first = *records[index % records.size()];
            const ith::TypeInfo& second = *records[(index + 1) % records.size()];
            const ith::Entity entity = world.spawn(ith::test::Health{.value = static_cast<int>(index)});
            world.add(entity, first);
            world.add(entity, second);
            world.insertResource(ith::test::GameClock{.frame = static_cast<std::uint64_t>(index)});
            world.insertResource(ith::typeOf<ith::test::MatchRules>());
            const std::array terms{ith::QueryTerm{.type = &first}, ith::QueryTerm{.type = &second}};
            int sum = 0;
            world.dynamicQuery(terms).forEach([&](ith::Entity, std::span<void* const> values) {
                sum += *static_cast<const int*>(values[0]) + *static_cast<const int*>(values[1]);
            });
            const auto expected = static_cast<int>((index % records.size()) + ((index + 1) % records.size()));
            if (sum != expected || world.resource<ith::test::GameClock>()->frame != index ||
                world.resource<ith::test::MatchRules>() == nullptr) {
                failures.fetch_add(1);
            }
        }
    });
    CHECK(failures.load() == 0);
}

TEST_CASE("World: a world handed from job to job keeps its entities and resources") {
    // One thread at a time uses the world; the waits order the jobs that use it.
    ith::JobSystem jobs(ith::JobSystemConfig{.foregroundWorkers = 2, .backgroundWorkers = 0});
    ith::World world;
    ith::JobCounter counter;
    jobs.submit(
        ith::JobLane::Foreground, "Fill",
        [&world] {
            for (int index = 0; index < 100; ++index) {
                world.spawn(ith::test::Health{.value = index});
            }
            world.insertResource(ith::test::GameClock{.frame = 1});
        },
        &counter);
    jobs.wait(counter);
    jobs.submit(
        ith::JobLane::Foreground, "Update",
        [&world] {
            const ith::TypeInfo& health = ith::typeOf<ith::test::Health>();
            const std::array terms{ith::QueryTerm{.type = &health, .access = ith::Access::Write}};
            world.dynamicQuery(terms).forEach([](ith::Entity, std::span<void* const> values) {
                static_cast<ith::test::Health*>(values[0])->value += 1;
            });
            world.resource<ith::test::GameClock>()->frame += 1;
        },
        &counter);
    jobs.wait(counter);
    int sum = 0;
    world.query<const ith::test::Health>().forEach([&](const ith::test::Health& health) { sum += health.value; });
    CHECK(sum == 5050);
    CHECK(world.resource<ith::test::GameClock>()->frame == 2);
}
