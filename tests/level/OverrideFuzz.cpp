// libFuzzer target for override records fed into World::applyPatches (ADR-0033 rules 12, 13 and
// 15, ADR-0037 rule 8): each input line is a record "<component path> <JSON value>", "+<type>
// <JSON value>" or "-<type>", or a whole-entity record "spawn", "spawn child" or "despawn <1-4>".
// The path is parsed through a registry of the engine and test components, the value is read
// through fromValue with an entity mapping to a small world, and the records become one
// applyPatches call on that world: component changes on two of its entities, then Spawns of new
// entities (under the root for "spawn child") and Despawns. The changes are applied as one step or
// not at all: a failure must leave the world as it was. The returned inverse must apply and restore
// the world exactly, hook side effects included, and its own inverse must redo the change exactly;
// only a despawned root, which another entity refers to, is exempt from the exact checks. Built only
// in Clang sanitizer builds; CTest runs a short smoke run seeded with tests/level/override_seeds,
// and longer runs are started by hand:
//
//   ith_override_fuzz -max_total_time=600 -dict=tests/level/level.dict <corpus directory>
//       tests/level/override_seeds

#include "ith/ecs/EntityPatch.h"
#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/foundation/Uuid.h"
#include "ith/level/Level.h"
#include "ith/reflection/OwnedValue.h"
#include "ith/reflection/Patch.h"
#include "ith/reflection/PropertyPath.h"
#include "ith/reflection/TypeOf.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/serialization/Json.h"
#include "ith/serialization/TextArchive.h"

#include "LevelComponents.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

ITH_DECLARE_REFLECTION_MODULE(ecs)
ITH_DECLARE_REFLECTION_MODULE(test_level)

namespace {

const ith::TypeRegistry& registry() {
    static const ith::TypeRegistry kRegistry = [] {
        ith::TypeRegistry result;
        ith::generated::registerReflection_ecs(result);
        ith::generated::registerReflection_test_level(result);
        return result;
    }();
    return kRegistry;
}

constexpr ith::Uuid kLevel(0x0FE5, 1);
constexpr std::size_t kMaxRecords = 16;

// The entities of the world by the ids Uuid(0, 1) to Uuid(0, 4); any other id reads as null.
struct Ids {
    const ith::World* world = nullptr;
};

ith::Entity toEntity(void* context, const ith::Uuid& id) {
    const auto& ids = *static_cast<const Ids*>(context);
    return ids.world->findByStableId(id);
}

std::string save(const ith::World& world) {
    const auto text = ith::saveLevel(world, kLevel, registry());
    if (!text) {
        std::abort(); // every value in the world was read or checked
    }
    return *text;
}

// One record as a change, or nothing when it doesn't parse or read.
bool makeChange(std::string_view line, const ith::serialization::EntityMapping& mapping,
                std::vector<ith::ComponentChange>& changes) {
    const std::size_t space = line.find(' ');
    const std::string_view key = line.substr(0, space);
    const std::string_view json = space == std::string_view::npos ? std::string_view() : line.substr(space + 1);
    if (key.empty()) {
        return false;
    }
    const bool add = key.front() == '+';
    const bool remove = key.front() == '-';
    const auto path = ith::PropertyPath::parseQualified(registry(), (add || remove) ? key.substr(1) : key);
    if (!path.hasValue() || ((add || remove) && !path->empty())) {
        return false;
    }
    if (remove) {
        changes.push_back(ith::ComponentChange::makeRemove(path->rootType()));
        return true;
    }
    const auto parsed = ith::serialization::parseJson(json);
    if (!parsed.hasValue()) {
        return false;
    }
    ith::OwnedValue value(path->leafType());
    if (!ith::serialization::fromValue(*parsed, path->leafType(), value.get(), {.entities = &mapping}).hasValue()) {
        return false;
    }
    if (add) {
        changes.push_back(ith::ComponentChange::makeAdd(std::move(value)));
    } else {
        ith::Patch patch(path->rootType());
        patch.set(*path, std::move(value));
        changes.push_back(ith::ComponentChange::makePatch(std::move(patch)));
    }
    return true;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using ith::test::level::Light;
    using ith::test::level::Sealed;
    using ith::test::level::Target;

    ith::World world;
    const ith::Entity root = world.spawn(ith::StableId{.id = ith::Uuid(0, 1)}, Light{.intensity = 2.0f},
                                         ith::LocalTransform{.value = {.translation = {1, 2, 3}}});
    const ith::Entity child = world.spawn(ith::StableId{.id = ith::Uuid(0, 2)}, ith::ChildOf{.parent = root},
                                          Target{.entity = root}, Sealed{.value = 7});
    world.spawn(ith::StableId{.id = ith::Uuid(0, 3)}, ith::ChildOf{.parent = child});
    world.spawn(ith::StableId{.id = ith::Uuid(0, 4)}, ith::Disabled{});

    Ids ids{.world = &world};
    const ith::serialization::EntityMapping mapping{.toEntity = &toEntity, .context = &ids};
    // Records alternate between the root and the child, so changes interleave across entities;
    // "spawn", "spawn child" and "despawn <n>" records append whole-entity patches after them.
    std::vector<ith::EntityPatch> patches{{.entity = root}, {.entity = child}};
    std::uint32_t spawns = 0;
    bool despawnsReferenced = false;
    std::string_view text(reinterpret_cast<const char*>(data), size);
    for (std::size_t record = 0; record < kMaxRecords && !text.empty(); ++record) {
        const std::size_t end = text.find('\n');
        const std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (line == "spawn" || line == "spawn child") {
            ith::EntityPatch& spawn = patches.emplace_back(
                ith::EntityPatch{.op = ith::EntityOp::Spawn, .entity = ith::Entity::fromParts(spawns, 1, 0)});
            spawn.changes.push_back(
                ith::ComponentChange::makeAdd(ith::OwnedValue::of(ith::StableId{.id = ith::Uuid(0, 100 + spawns)})));
            if (line == "spawn child") {
                spawn.changes.push_back(
                    ith::ComponentChange::makeAdd(ith::OwnedValue::of(ith::ChildOf{.parent = root})));
            }
            ++spawns;
            continue;
        }
        if (line.size() == 9 && line.starts_with("despawn ") && line[8] >= '1' && line[8] <= '4') {
            const auto id = static_cast<std::uint64_t>(line[8] - '0');
            // Target on the child refers to the root: after an undo it stays stale (EntityPatch.h).
            despawnsReferenced = despawnsReferenced || id == 1;
            patches.push_back(
                ith::EntityPatch{.op = ith::EntityOp::Despawn, .entity = world.findByStableId(ith::Uuid(0, id))});
            continue;
        }
        static_cast<void>(makeChange(line, mapping, patches[record % 2].changes));
    }

    const std::string before = save(world);
    const auto inverse = world.applyPatches(patches);
    if (!inverse.hasValue()) {
        if (save(world) != before) {
            std::abort(); // a failed apply changed the world
        }
        return 0;
    }
    const std::string after = save(world);
    const auto redo = world.applyPatches(*inverse);
    if (!redo.hasValue()) {
        std::abort(); // the inverse of an applied change must apply
    }
    if (despawnsReferenced) {
        return 0;
    }
    if (save(world) != before) {
        std::abort(); // the inverse must restore every saved value, hook side effects included
    }
    if (!world.applyPatches(*redo).hasValue() || save(world) != after) {
        std::abort(); // the inverse of the inverse must redo the change exactly
    }
    return 0;
}
