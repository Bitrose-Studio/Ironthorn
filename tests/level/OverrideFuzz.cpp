// libFuzzer target for override records fed into World::applyPatches (ADR-0033 rules 12, 13 and
// 15): each input line is a record "<component path> <JSON value>", "+<type> <JSON value>" or
// "-<type>". The path is parsed through a registry of the engine and test components, the value is
// read through fromValue with an entity mapping to a small world, and the records become one
// applyPatches call on two entities of that world: a patch with a set, an add or a remove. The
// changes are applied as one step or not at all: a failure must leave the world as it was, and the
// returned inverse must apply and, when no ChildOf changed (whose hooks recompute LocalTransform),
// restore the world exactly. Built only in Clang sanitizer builds; CTest runs a short smoke run
// seeded with tests/level/override_seeds, and longer runs are started by hand:
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
                std::vector<ith::ComponentChange>& changes, bool& touchesParent) {
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
    touchesParent = touchesParent || path->rootType().id == ith::typeOf<ith::ChildOf>().id;
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
    // Records alternate between the root and the child, so changes interleave across entities.
    std::vector<ith::EntityPatch> patches{{.entity = root}, {.entity = child}};
    bool touchesParent = false;
    std::string_view text(reinterpret_cast<const char*>(data), size);
    for (std::size_t record = 0; record < kMaxRecords && !text.empty(); ++record) {
        const std::size_t end = text.find('\n');
        static_cast<void>(makeChange(text.substr(0, end), mapping, patches[record % 2].changes, touchesParent));
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
    }

    const std::string before = save(world);
    const auto inverse = world.applyPatches(patches);
    if (!inverse.hasValue()) {
        if (save(world) != before) {
            std::abort(); // a failed apply changed the world
        }
        return 0;
    }
    if (!world.applyPatches(*inverse).hasValue()) {
        std::abort(); // the inverse of an applied change must apply
    }
    if (!touchesParent && save(world) != before) {
        std::abort(); // the inverse must restore every saved value
    }
    return 0;
}
