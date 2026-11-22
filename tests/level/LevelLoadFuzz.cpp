// libFuzzer target for loadLevel (ADR-0034): arbitrary bytes are loaded as a level, as the editor
// loads (unknown fields and components as warnings) and as the cooker loads (as errors), into a
// fresh world and into a world that already has entities, hooked components and a resource, so
// that clearing and refilling is covered. A level that loads is saved, loaded again and saved
// again, and both saves must give the same bytes; a failed load must leave the filled world as it
// was. Built only in Clang sanitizer builds; CTest runs a short smoke run seeded with
// tests/level/fuzz_seeds and the dictionary tests/level/level.dict, and longer runs are started by
// hand:
//
//   ith_level_fuzz -max_total_time=600 -dict=tests/level/level.dict <corpus directory>
//       tests/level/fuzz_seeds

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/Hooks.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/foundation/Uuid.h"
#include "ith/level/Level.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/serialization/TextArchive.h"

#include "LevelComponents.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

ITH_DECLARE_REFLECTION_MODULE(ecs)
ITH_DECLARE_REFLECTION_MODULE(level)
ITH_DECLARE_REFLECTION_MODULE(test_level)
ITH_DECLARE_REFLECTION_MODULE(test_level_editor)

namespace {

using ith::test::level::LevelSettings;
using ith::test::level::Marker;
using ith::test::level::Probe;

// Probe's hooks: each add counts into the world's LevelSettings, and each removal (the clear before
// a load) spawns an entity without a StableId through the hook's commands, which survives the clear.
void setProbeHooks() {
    static const bool kSet = ith::setComponentHooks<Probe>({
        .onAdd =
            [](ith::HookContext& context) {
                if (auto* settings = context.resource<LevelSettings>()) {
                    settings->gravity += 1.0f;
                }
            },
        .onRemove = [](ith::HookContext& context) { context.commands().spawn(Marker{}); },
    });
    if (!kSet) {
        std::abort();
    }
}

const ith::TypeRegistry& registry() {
    static const ith::TypeRegistry kRegistry = [] {
        setProbeHooks();
        ith::TypeRegistry result;
        ith::generated::registerReflection_ecs(result);
        ith::generated::registerReflection_level(result);
        ith::generated::registerReflection_test_level(result);
        ith::generated::registerReflection_test_level_editor(result);
        return result;
    }();
    return kRegistry;
}

constexpr ith::Uuid kFilledLevel(0xF111ED, 1);

// A world that already holds a small level: a hierarchy, hooked components, an entity without a
// StableId and a resource. Some of its stable ids are the seeds' ids, so a load replaces them.
void fill(ith::World& world) {
    world.insertResource(LevelSettings{.gravity = -9.81f});
    const ith::Entity root = world.spawn(ith::StableId{.id = ith::Uuid(0, 1)}, Probe{.value = 1});
    const ith::Entity child =
        world.spawn(ith::StableId{.id = ith::Uuid(0, 2)}, ith::ChildOf{.parent = root}, Probe{.value = 2});
    world.spawn(ith::ChildOf{.parent = child}, Marker{});
    world.spawn(ith::StableId{.id = ith::Uuid(0xF111ED, 3)}, ith::Disabled{});
}

// Saves `world`; a save of a loaded level can't fail, since everything in it was read from text.
std::string save(const ith::World& world, const ith::Uuid& id) {
    const auto text = ith::saveLevel(world, id, registry());
    if (!text) {
        std::abort();
    }
    return *text;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    {
        ith::World world;
        static_cast<void>(
            ith::loadLevel(text, world, registry(), {.unknownFields = ith::serialization::UnknownFields::Fail}));
    }

    // Into a filled world: a failed load leaves it as it was, a successful one keeps its resource.
    {
        ith::World filled;
        fill(filled);
        const std::string before = save(filled, kFilledLevel);
        const std::size_t entities = filled.entityCount();
        std::vector<ith::serialization::Error> warnings;
        const auto info = ith::loadLevel(text, filled, registry(), {.warnings = &warnings});
        if (!info) {
            if (filled.entityCount() != entities || save(filled, kFilledLevel) != before) {
                std::abort();
            }
        } else if (filled.resource<const LevelSettings>() == nullptr) {
            std::abort();
        }
        if (warnings.size() > 8 * (ith::kMaxLevelWarningsPerCode + 1)) {
            std::abort(); // a handful of codes, each capped
        }
    }

    ith::World world;
    const auto info = ith::loadLevel(text, world, registry());
    if (!info) {
        return 0;
    }
    const std::string first = save(world, info->id);
    ith::World again;
    if (!ith::loadLevel(first, again, registry()).hasValue() || save(again, info->id) != first) {
        std::abort();
    }
    return 0;
}
