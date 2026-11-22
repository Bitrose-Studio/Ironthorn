#pragma once

// ith::Schedule: systems in stages, run in a defined order (ADR-0014 rules 16 to 18).
//
//   ith::Schedule schedule;
//   schedule.add({
//       .name = "game::integrate",
//       .stage = ith::Stage::PrePhysics,
//       .access = ith::accessTo<Position, const Velocity, const GameClock>(),
//       .after = {"game::steer"},
//       .run = [](ith::SystemContext& context) {
//           const float delta = context.resource<const GameClock>()->delta;
//           context.query<Position, const Velocity>().forEach([&](Position& p, const Velocity& v) {
//               p.x += v.x * delta;
//           });
//       },
//   });
//   if (auto built = schedule.build(); !built) { report(built.error().message); }
//   schedule.run(world); // once per frame
//
// A system is a descriptor: a unique name, its stage, the component and resource types it reads
// and writes, `before`/`after` constraints on other systems of its stage, and the function that
// runs it. Systems hold logic and declare all the data they touch, so the schedule can order them
// and, later, run independent ones in parallel without an API change.
//
// Stages run in the order of ith::Stage, each ending with a sync point where the commands its
// systems recorded are applied: system by system in run order, each system's commands in
// recording order. Spawns, despawns, adds and removes made in a stage are therefore visible from
// the next stage on, and the result doesn't depend on how the systems were executed.
//
// Order within a stage: every `before`/`after` constraint holds and, among the systems whose
// constraints are met, the one added first runs first; without constraints, systems run in the
// order they were added. The executor is serial and deterministic: the same systems, added in the
// same order, run in the same order on every run and every machine.
//
// build() checks everything before anything runs and fails with an ith::Error when a name is empty
// or used twice, a system has no function, a constraint names an unknown system or one of another
// stage, the constraints form a cycle, or an access entry is not a component or resource type,
// names a type twice or writes an immutable component type (ADR-0030). Codes: "ecs.schedule.invalid_system", "ecs.schedule.duplicate_name",
// "ecs.schedule.unknown_system", "ecs.schedule.other_stage", "ecs.schedule.cycle",
// "ecs.schedule.invalid_access". The message names the systems involved.
//
// Declared access is enforced: a system that queries, gets or reads a component or resource it
// didn't declare, or writes one it declared as read-only, asserts (in builds with asserts). While
// a system runs, the world's structure can't change directly (asserted and refused): record
// commands instead.
//
// Not thread-safe: run a schedule on the thread that owns the World. A system may use jobs
// internally, giving each job its own command buffer (Commands.h).

#include "ith/ecs/Commands.h"
#include "ith/ecs/ComponentRegistry.h"
#include "ith/ecs/DynamicQuery.h"
#include "ith/ecs/Entity.h"
#include "ith/ecs/Query.h"
#include "ith/ecs/World.h"
#include "ith/foundation/Expected.h"
#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeOf.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ith {

class SystemContext;

namespace detail {
// The run function of transformPropagationSystem (TransformPropagation.h).
void runTransformPropagation(const SystemContext& context);
} // namespace detail

// The stages of a frame, in run order (ADR-0014 rule 17). Each ends with a sync point.
enum class Stage : std::uint8_t {
    Input,         // input routing (ADR-0013)
    PrePhysics,    // ordinary gameplay
    DuringPhysics, // work that overlaps the physics step
    PostPhysics,   // reactions to the physics results
    PostUpdate,    // transform propagation and other follow-up work
    Extract,       // filling the renderer's data
};

inline constexpr std::size_t kStageCount = 6;

// The stage's name as written in the enum, e.g. "PrePhysics".
[[nodiscard]] std::string_view stageName(Stage stage) noexcept;

// The access list of a system from C++ types: `const T` reads T, plain `T` writes it. Each type is
// a component or a resource type.
//
//   .access = ith::accessTo<Position, const Velocity, const GameClock>()
template <typename... Types>
[[nodiscard]] std::vector<QueryTerm> accessTo() {
    static_assert((!std::is_reference_v<Types> && ...), "access names types, not references");
    return {QueryTerm{.type = &typeOf<std::remove_const_t<Types>>(),
                      .access = std::is_const_v<Types> ? Access::Read : Access::Write}...};
}

// A system's function. It runs on the thread that runs the schedule, once per Schedule::run. Being a
// std::function, it must be copyable: keep state that can't be copied behind a pointer.
using SystemFunction = std::function<void(SystemContext&)>;

// A system as Schedule::add takes it. build() checks every field.
struct SystemDescriptor {
    std::string name; // unique within the schedule, e.g. "game::integrate"
    Stage stage = Stage::PrePhysics;
    std::vector<QueryTerm> access;   // every component and resource type the system touches
    std::vector<std::string> before; // systems of the same stage that run after this one
    std::vector<std::string> after;  // systems of the same stage that run before this one
    SystemFunction run;
};

namespace detail {

// A system's declared access, resolved by build().
struct SystemAccess {
    struct Component {
        ComponentIndex type = kNoComponent;
        Access access = Access::Read;
    };

    struct Resource {
        std::uint64_t id = 0;
        Access access = Access::Read;
    };

    std::vector<Component> components;
    std::vector<Resource> resources;
};

} // namespace detail

// What a running system works with: queries, components, resources and its command buffer, all
// checked against its declared access. Valid only during the system's run.
class SystemContext {
public:
    SystemContext(const SystemContext&) = delete;
    SystemContext& operator=(const SystemContext&) = delete;
    SystemContext(SystemContext&&) = delete;
    SystemContext& operator=(SystemContext&&) = delete;
    ~SystemContext() = default;

    // World::query; each term must be declared, `T` with write access. Filters (With, Without)
    // read no values and need no access.
    template <typename... Terms>
    [[nodiscard]] detail::QueryOf<Terms...> query();

    // World::dynamicQuery; each term must be declared, a write term with write access. Filters
    // need no access.
    [[nodiscard]] DynamicQuery dynamicQuery(std::span<const QueryTerm> terms,
                                            std::span<const QueryFilter> filters = {});

    // World::get; T must be declared, with write access unless T is const.
    template <typename T>
    [[nodiscard]] T* get(Entity entity);

    // World::has and World::isAlive. They read no component values, so they need no access.
    template <typename T>
    [[nodiscard]] bool has(Entity entity) const;
    [[nodiscard]] bool isAlive(Entity entity) const;

    // World::resource; T must be declared, with write access unless T is const.
    template <typename T>
    [[nodiscard]] T* resource();

    // The system's command buffer, applied at the end of its stage.
    [[nodiscard]] Commands& commands() noexcept { return *m_commands; }

    // A new, empty buffer for this world, e.g. one per job; append it to commands() to apply it.
    [[nodiscard]] Commands makeCommands() const noexcept;

    // The running system's name.
    [[nodiscard]] std::string_view systemName() const noexcept { return m_name; }

private:
    friend class Schedule;
    friend void detail::runTransformPropagation(const SystemContext& context);

    SystemContext(World& world, Commands& commands, std::string_view name, const detail::SystemAccess& access) noexcept
        : m_world(&world), m_commands(&commands), m_name(name), m_access(&access) {}

    // Assert that the access is declared.
    void checkComponent(detail::ComponentIndex type, Access access) const;
    void checkResource(const TypeInfo& type, Access access) const;

    World* m_world;
    Commands* m_commands;
    std::string_view m_name;
    [[maybe_unused]] const detail::SystemAccess* m_access; // read by the access checks, which need asserts
};

class Schedule {
public:
    // Adds a system. The schedule needs build() again before it runs.
    void add(SystemDescriptor system);

    // Checks the systems and fixes their order (see the header comment).
    [[nodiscard]] Expected<void> build();

    // Runs every stage in order, each followed by its sync point. Precondition (asserted; nothing
    // runs otherwise): build() succeeded since the last add(), the world is not being iterated, and
    // the schedule is not running already. Each system's command buffer is kept for the next run;
    // running the schedule on another world replaces them.
    void run(World& world);

    // The names of a stage's systems in run order, after a successful build(); empty before.
    [[nodiscard]] std::vector<std::string_view> order(Stage stage) const;

    // The number of systems added.
    [[nodiscard]] std::size_t systemCount() const noexcept { return m_systems.size(); }

private:
    struct System {
        SystemDescriptor descriptor;
        detail::SystemAccess access;
    };

    std::vector<System> m_systems; // in the order they were added
    std::array<std::vector<std::uint32_t>, kStageCount> m_order;
    std::vector<Commands> m_commands; // one per system, for m_commandsWorld
    World* m_commandsWorld = nullptr;
    bool m_built = false;
    bool m_running = false;
};

template <typename... Terms>
detail::QueryOf<Terms...> SystemContext::query() {
    const auto check = [this]<typename Term> {
        if constexpr (!detail::kIsFilter<Term>) {
            checkComponent(detail::componentIndexOf<std::remove_const_t<Term>>(),
                           std::is_const_v<Term> ? Access::Read : Access::Write);
        }
    };
    (check.template operator()<Terms>(), ...);
    return m_world->query<Terms...>();
}

template <typename T>
T* SystemContext::get(Entity entity) {
    checkComponent(detail::componentIndexOf<std::remove_const_t<T>>(),
                   std::is_const_v<T> ? Access::Read : Access::Write);
    return m_world->get<T>(entity);
}

template <typename T>
bool SystemContext::has(Entity entity) const {
    return m_world->has<T>(entity);
}

template <typename T>
T* SystemContext::resource() {
    checkResource(typeOf<std::remove_const_t<T>>(), std::is_const_v<T> ? Access::Read : Access::Write);
    return m_world->resource<T>();
}

} // namespace ith
