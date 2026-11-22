#include "ith/ecs/Schedule.h"

#include "ith/foundation/Assert.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <queue>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

namespace ith {
namespace {

constexpr std::array<std::string_view, kStageCount> kStageNames = {
    "Input", "PrePhysics", "DuringPhysics", "PostPhysics", "PostUpdate", "Extract",
};

std::string quoted(std::string_view name) {
    return "'" + std::string(name) + "'";
}

// Resolves a system's access list. Fails for a null type, a type that is neither a component nor
// a resource, a type named twice, and write access to an immutable component type.
Expected<detail::SystemAccess> resolveAccess(const SystemDescriptor& system) {
    detail::SystemAccess access;
    std::vector<std::uint64_t> seen;
    for (const QueryTerm& term : system.access) {
        const TypeInfo* type = term.type;
        if (type == nullptr || type->kind != TypeKind::Struct ||
            (type->role != TypeRole::Component && type->role != TypeRole::Resource)) {
            return makeError("ecs.schedule.invalid_access",
                             "system " + quoted(system.name) +
                                 " declares access to a type that is not a component or resource type");
        }
        if (std::ranges::find_if(seen, [&](std::uint64_t id) { return id == type->id; }) != seen.end()) {
            return makeError("ecs.schedule.invalid_access",
                             "system " + quoted(system.name) + " declares " + quoted(type->name) + " twice");
        }
        seen.push_back(type->id);
        if (type->role == TypeRole::Component && term.access == Access::Write &&
            hasFlags(type->flags, ReflectionFlags::Immutable)) {
            return makeError("ecs.schedule.invalid_access",
                             "system " + quoted(system.name) + " declares write access to " + quoted(type->name) +
                                 ", which is immutable; read it and replace it with a command");
        }
        if (type->role == TypeRole::Component) {
            access.components.push_back({detail::registerComponentType(*type), term.access});
        } else {
            access.resources.push_back({type->id, term.access});
        }
    }
    return access;
}

using NameIndex = std::unordered_map<std::string_view, std::uint32_t>;

// Checks a system's name, function and stage, and records its name in `byName`.
Expected<void> checkSystem(const SystemDescriptor& descriptor, std::uint32_t index, NameIndex& byName) {
    if (descriptor.name.empty()) {
        return makeError("ecs.schedule.invalid_system", "a system has no name");
    }
    if (!descriptor.run || static_cast<std::size_t>(descriptor.stage) >= kStageCount) {
        return makeError("ecs.schedule.invalid_system",
                         "system " + quoted(descriptor.name) + " has no function or an invalid stage");
    }
    if (!byName.emplace(descriptor.name, index).second) {
        return makeError("ecs.schedule.duplicate_name", "two systems are named " + quoted(descriptor.name));
    }
    return {};
}

// The order constraints as edges: from each system to the systems that must run after it.
struct Graph {
    std::vector<std::vector<std::uint32_t>> successors;
    std::vector<std::uint32_t> predecessorCount;
};

Expected<Graph> buildGraph(std::span<const SystemDescriptor* const> systems, const NameIndex& byName) {
    Graph graph{.successors = std::vector<std::vector<std::uint32_t>>(systems.size()),
                .predecessorCount = std::vector<std::uint32_t>(systems.size(), 0)};
    for (std::uint32_t index = 0; index < systems.size(); ++index) {
        const SystemDescriptor& descriptor = *systems[index];
        const auto constrain = [&](const std::string& other, bool otherRunsAfter) -> Expected<void> {
            const auto found = byName.find(other);
            if (found == byName.end()) {
                return makeError("ecs.schedule.unknown_system", "system " + quoted(descriptor.name) +
                                                                    " is ordered against an unknown system " +
                                                                    quoted(other));
            }
            const std::uint32_t otherIndex = found->second;
            if (systems[otherIndex]->stage != descriptor.stage) {
                return makeError("ecs.schedule.other_stage", "system " + quoted(descriptor.name) +
                                                                 " is ordered against " + quoted(other) +
                                                                 ", which runs in another stage");
            }
            const std::uint32_t first = otherRunsAfter ? index : otherIndex;
            const std::uint32_t second = otherRunsAfter ? otherIndex : index;
            graph.successors[first].push_back(second);
            ++graph.predecessorCount[second];
            return {};
        };
        for (const std::string& other : descriptor.before) {
            if (Expected<void> result = constrain(other, true); !result) {
                return makeUnexpected(std::move(result.error()));
            }
        }
        for (const std::string& other : descriptor.after) {
            if (Expected<void> result = constrain(other, false); !result) {
                return makeUnexpected(std::move(result.error()));
            }
        }
    }
    return graph;
}

// Kahn's algorithm over one stage; among the systems ready to run, the one added first goes first.
// Fails with the systems left over when the constraints form a cycle.
Expected<std::vector<std::uint32_t>> orderStage(std::size_t stage, std::span<const SystemDescriptor* const> systems,
                                                Graph& graph) {
    const auto inStage = [&](std::uint32_t index) { return static_cast<std::size_t>(systems[index]->stage) == stage; };
    std::priority_queue<std::uint32_t, std::vector<std::uint32_t>, std::greater<>> ready;
    std::size_t stageSize = 0;
    for (std::uint32_t index = 0; index < systems.size(); ++index) {
        if (inStage(index)) {
            ++stageSize;
            if (graph.predecessorCount[index] == 0) {
                ready.push(index);
            }
        }
    }
    std::vector<std::uint32_t> order;
    while (!ready.empty()) {
        const std::uint32_t index = ready.top();
        ready.pop();
        order.push_back(index);
        for (const std::uint32_t next : graph.successors[index]) {
            if (--graph.predecessorCount[next] == 0) {
                ready.push(next);
            }
        }
    }
    if (order.size() == stageSize) {
        return order;
    }
    std::string names;
    for (std::uint32_t index = 0; index < systems.size(); ++index) {
        if (inStage(index) && graph.predecessorCount[index] != 0) {
            names += (names.empty() ? "" : ", ") + quoted(systems[index]->name);
        }
    }
    return makeError("ecs.schedule.cycle", "the order constraints of stage " + std::string(kStageNames[stage]) +
                                               " form a cycle through " + names);
}

} // namespace

std::string_view stageName(Stage stage) noexcept {
    const auto index = static_cast<std::size_t>(stage);
    return index < kStageCount ? kStageNames[index] : std::string_view("unknown");
}

void Schedule::add(SystemDescriptor system) {
    ITH_ASSERT_MSG(!m_running, "ecs: a system added while the schedule runs");
    // Each system gets a key no other system of the process has, under which every World keeps its
    // last run. Key 0 belongs to propagateTransforms called directly.
    static std::atomic<std::uint64_t> nextKey{1};
    m_systems.push_back(
        {.descriptor = std::move(system), .access = {}, .key = nextKey.fetch_add(1, std::memory_order_relaxed)});
    m_built = false;
}

Expected<void> Schedule::build() {
    m_built = false;
    for (std::vector<std::uint32_t>& order : m_order) {
        order.clear();
    }
    NameIndex byName;
    std::vector<const SystemDescriptor*> descriptors;
    for (std::uint32_t index = 0; index < m_systems.size(); ++index) {
        System& system = m_systems[index];
        if (Expected<void> checked = checkSystem(system.descriptor, index, byName); !checked) {
            return checked;
        }
        Expected<detail::SystemAccess> access = resolveAccess(system.descriptor);
        if (!access) {
            return makeUnexpected(std::move(access.error()));
        }
        system.access = std::move(*access);
        descriptors.push_back(&system.descriptor);
    }
    Expected<Graph> graph = buildGraph(descriptors, byName);
    if (!graph) {
        return makeUnexpected(std::move(graph.error()));
    }
    std::array<std::vector<std::uint32_t>, kStageCount> order;
    for (std::size_t stage = 0; stage < kStageCount; ++stage) {
        Expected<std::vector<std::uint32_t>> stageOrder = orderStage(stage, descriptors, *graph);
        if (!stageOrder) {
            return makeUnexpected(std::move(stageOrder.error()));
        }
        order[stage] = std::move(*stageOrder);
    }
    m_order = std::move(order);
    m_built = true;
    return {};
}

void Schedule::run(World& world) {
    ITH_ASSERT_MSG(m_built, "ecs: a schedule run without a successful build()");
    ITH_ASSERT_MSG(!m_running, "ecs: a schedule run from one of its own systems");
    ITH_ASSERT_MSG(world.m_iterationDepth == 0, "ecs: a schedule run while its world is iterated");
    if (!m_built || m_running || world.m_iterationDepth != 0) {
        return;
    }
    m_running = true;
    if (m_commandsWorld != &world || m_commands.size() != m_systems.size()) {
        m_commands.clear();
        m_commands.reserve(m_systems.size());
        for (std::size_t index = 0; index < m_systems.size(); ++index) {
            m_commands.emplace_back(world);
        }
        m_commandsWorld = &world;
    }
    for (const std::vector<std::uint32_t>& order : m_order) {
        for (const std::uint32_t index : order) {
            const System& system = m_systems[index];
            SystemContext context(world, m_commands[index], system.descriptor.name, system.access);
            const detail::ChangeClock outer = world.beginRun(system.key);
            {
                // Direct structural changes are refused while the system runs.
                const detail::IterationScope scope(world.m_iterationDepth);
                system.descriptor.run(context);
            }
            world.endRun(system.key, outer);
        }
        // The sync point: commands in system run order, each buffer in recording order.
        for (const std::uint32_t index : order) {
            world.apply(m_commands[index]);
        }
    }
    m_running = false;
}

std::vector<std::string_view> Schedule::order(Stage stage) const {
    std::vector<std::string_view> names;
    const auto index = static_cast<std::size_t>(stage);
    if (m_built && index < kStageCount) {
        for (const std::uint32_t system : m_order[index]) {
            names.emplace_back(m_systems[system].descriptor.name);
        }
    }
    return names;
}

DynamicQuery SystemContext::dynamicQuery(std::span<const QueryTerm> terms, std::span<const QueryFilter> filters) {
    for (const QueryTerm& term : terms) {
        if (term.type != nullptr && term.type->kind == TypeKind::Struct && term.type->role == TypeRole::Component) {
            checkComponent(detail::registerComponentType(*term.type), term.access);
        }
    }
    // Change filters read the type's ticks, which its writers change.
    for (const QueryFilter& filter : filters) {
        if ((filter.kind == FilterKind::Added || filter.kind == FilterKind::Changed) && filter.type != nullptr &&
            filter.type->kind == TypeKind::Struct && filter.type->role == TypeRole::Component) {
            checkComponent(detail::registerComponentType(*filter.type), Access::Read);
        }
    }
    return m_world->dynamicQuery(terms, filters); // asserts for invalid terms and filters
}

bool SystemContext::isAlive(Entity entity) const {
    return m_world->isAlive(entity);
}

Commands SystemContext::makeCommands() const noexcept {
    return Commands(*m_world);
}

void SystemContext::checkComponent([[maybe_unused]] detail::ComponentIndex type, [[maybe_unused]] Access access) const {
#ifdef ITH_ENABLE_ASSERTS
    const auto found = std::ranges::find_if(
        m_access->components, [&](const detail::SystemAccess::Component& entry) { return entry.type == type; });
    ITH_ASSERT_MSG(found != m_access->components.end(), "ecs: a system touched a component it didn't declare");
    ITH_ASSERT_MSG(found == m_access->components.end() || access == Access::Read || found->access == Access::Write,
                   "ecs: a system wrote a component it declared as read-only");
#endif
}

void SystemContext::checkResource([[maybe_unused]] const TypeInfo& type, [[maybe_unused]] Access access) const {
#ifdef ITH_ENABLE_ASSERTS
    const auto found = std::ranges::find_if(
        m_access->resources, [&](const detail::SystemAccess::Resource& entry) { return entry.id == type.id; });
    ITH_ASSERT_MSG(found != m_access->resources.end(), "ecs: a system touched a resource it didn't declare");
    ITH_ASSERT_MSG(found == m_access->resources.end() || access == Access::Read || found->access == Access::Write,
                   "ecs: a system wrote a resource it declared as read-only");
#endif
}

} // namespace ith
