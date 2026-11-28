// The Play In Editor benchmark (docs/specs/play-in-editor.md, requirements 46 to 48): a level of
// 10 000 entities, each with a StableId, an EditorLabel, a LocalTransform, a WorldTransform and a
// ShapeRenderer, a quarter of them children. The targets on the Low tier reference machine
// (ADR-0005): starting a session (the copy and the schedule build) in at most 100 ms, Stop in at
// most 50 ms, and the session's own work per tick (Time, the schedule's overhead and the view model
// polling, without the game systems) in at most 0.25 ms, allocating nothing in steady state. Run on
// a Release build, by hand or as the CTest `play_session_benchmark` (label "benchmark", excluded from
// normal test runs). It prints numbers and always exits with 0.
//
//   cmake --build build/windows --config Release --target ith_play_session_benchmark
//   build/windows/bin/Release/ith_play_session_benchmark
//
// The times are medians of repeated runs. A tick is measured as a whole (ith.play.step's tick, the
// Outliner and Details polling) and the engine's one game-world system, transform propagation, is
// measured alone on the same world; the session's own work is the difference. Allocations are
// counted by replacing the global operator new in this program. Details compares its shown values
// with its working copy after a tick and refreshes only when one changed; a tick that does change a
// shown value still rebuilds the working copy and rows (allocating), as updating them in place is
// deferred.

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/Schedule.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/DetailsModel.h"
#include "ith/editor_core/EditorLabel.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/OutlinerModel.h"
#include "ith/editor_core/PlaySession.h"
#include "ith/foundation/Stopwatch.h"
#include "ith/foundation/Uuid.h"
#include "ith/gameplay/GameWorld.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/shapes/ShapeRenderer.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

ITH_DECLARE_REFLECTION_MODULE(ecs)
ITH_DECLARE_REFLECTION_MODULE(editor_core)
ITH_DECLARE_REFLECTION_MODULE(shapes)

namespace {

std::atomic<std::size_t> g_allocations{0};

} // namespace

// Counts every allocation of the program; the aligned and nothrow forms end up here or keep their own
// pairs.
void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    std::abort(); // out of memory: the engine has no exceptions (ADR-0001)
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}

namespace {

using namespace ith;

constexpr int kEntities = 10'000;
constexpr int kStartRuns = 21;
constexpr int kTickRuns = 501;
constexpr double kStartTargetMs = 100.0;
constexpr double kStopTargetMs = 50.0;
constexpr double kTickTargetMs = 0.25;

double median(std::vector<double> samples) {
    std::ranges::sort(samples);
    return samples[samples.size() / 2];
}

void report(const char* what, double milliseconds, double target) {
    std::printf("  %-52s %8.3f ms", what, milliseconds);
    if (target > 0.0) {
        std::printf("  (target %.2f ms: %s)", target, milliseconds <= target ? "met" : "MISSED");
    }
    std::printf("\n");
}

// The level of requirement 46, built straight into the editor world.
void buildLevel(World& world) {
    std::vector<Entity> roots;
    for (int i = 0; i < kEntities; ++i) {
        const Entity entity =
            world.spawn(StableId{.id = Uuid::generate()}, EditorLabel{.name = "Entity " + std::to_string(i)},
                        LocalTransform{}, WorldTransform{}, ShapeRenderer{});
        // Every fourth entity is the child of a root.
        if (i % 4 == 3 && !roots.empty()) {
            world.add(entity, ChildOf{.parent = roots[static_cast<std::size_t>(i) % roots.size()]});
        } else {
            roots.push_back(entity);
        }
    }
}

} // namespace

int main() {
    TypeRegistry types;
    generated::registerReflection_ecs(types);
    generated::registerReflection_editor_core(types);
    generated::registerReflection_shapes(types);
    EditorSession session(types);
    buildLevel(session.document().mutableWorld());
    session.document().selection().replace({});
    std::printf("Play In Editor benchmark: %d entities, a quarter of them children\n", kEntities);

    std::vector<double> starts;
    std::vector<double> stops;
    for (int run = 0; run < kStartRuns; ++run) {
        Stopwatch stopwatch;
        if (Expected<void> started = session.startPlay(PlayMode::Simulate, true); !started) {
            std::printf("  Play failed: %s; skipped\n", started.error().message.c_str());
            return 0;
        }
        starts.push_back(stopwatch.elapsedMilliseconds());
        stopwatch.restart();
        session.stopPlay();
        stops.push_back(stopwatch.elapsedMilliseconds());
    }
    report("start (copy, Time, schedule build)", median(starts), kStartTargetMs);
    report("stop", median(stops), kStopTargetMs);

    // A running session with the Outliner and Details open on one selected entity.
    if (!session.startPlay(PlayMode::Simulate, true)) {
        return 0;
    }
    PlaySession& play = *session.play();
    std::vector<Uuid> first;
    play.mutableWorld().forEachRoot([&](Entity root) {
        if (first.empty()) {
            first.push_back(play.world().get<const StableId>(root)->id);
        }
    });
    play.selection().replace(first);
    play.changes().bumpSelection();
    OutlinerModel outliner(session);
    DetailsModel details(session);
    outliner.update();
    details.update();
    // Steady state first: the schedule's buffers, the transforms' first propagation.
    for (int warm = 0; warm < 20; ++warm) {
        play.step(1, 1.0 / 60.0);
        outliner.update();
        details.update();
    }

    std::vector<double> ticks;
    std::vector<double> sessionOnly;
    std::size_t tickAllocations = 0;
    std::size_t sessionAllocations = 0;
    const std::uint64_t rebuildsBefore = outliner.rebuilds();
    const std::uint64_t refreshesBefore = details.refreshes();
    for (int run = 0; run < kTickRuns; ++run) {
        std::size_t allocations = g_allocations.load(std::memory_order_relaxed);
        Stopwatch stopwatch;
        play.step(1, 1.0 / 60.0);
        sessionOnly.push_back(stopwatch.elapsedMilliseconds());
        sessionAllocations += g_allocations.load(std::memory_order_relaxed) - allocations;
        allocations = g_allocations.load(std::memory_order_relaxed);
        outliner.update();
        details.update();
        ticks.push_back(stopwatch.elapsedMilliseconds());
        tickAllocations += g_allocations.load(std::memory_order_relaxed) - allocations;
    }

    // The engine's game-world system alone on the same world: what a tick costs without the session.
    Schedule bare;
    if (!buildGameSchedule(bare, {})) {
        return 0;
    }
    std::vector<double> propagation;
    for (int run = 0; run < kTickRuns; ++run) {
        Stopwatch stopwatch;
        bare.run(play.mutableWorld());
        propagation.push_back(stopwatch.elapsedMilliseconds());
    }
    const double tick = median(ticks);
    const double systems = median(propagation);
    report("tick with view model polling (step, Outliner, Details)", tick, 0.0);
    report("  of which the tick alone (Time, schedule, counters)", median(sessionOnly), 0.0);
    report("transform propagation alone (the game-world system)", systems, 0.0);
    report("session's own work per tick", std::max(tick - systems, 0.0), kTickTargetMs);
    std::printf("  allocations per tick: %.2f in the tick, %.2f in the view models' polling\n",
                static_cast<double>(sessionAllocations) / kTickRuns, static_cast<double>(tickAllocations) / kTickRuns);
    std::printf("  Outliner rebuilds over %d ticks: %llu; Details refreshes: %llu (of %zu shown entity)\n", kTickRuns,
                static_cast<unsigned long long>(outliner.rebuilds() - rebuildsBefore),
                static_cast<unsigned long long>(details.refreshes() - refreshesBefore), details.entities().size());
    session.stopPlay();
    return 0;
}
