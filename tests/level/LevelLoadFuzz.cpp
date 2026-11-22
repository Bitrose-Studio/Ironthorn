// libFuzzer target for loadLevel (ADR-0034): arbitrary bytes are loaded as a level into a world, as
// the editor loads (unknown fields and components as warnings) and as the cooker loads (as errors).
// A level that loads is saved, loaded again and saved again, and both saves must give the same
// bytes. Built only in Clang sanitizer builds; CTest runs a short smoke run seeded with
// tests/level/fuzz_seeds and the JSON dictionary, and longer runs are started by hand:
//
//   ith_level_fuzz -max_total_time=600 -dict=tests/serialization/fuzz_seeds/json.dict
//       <corpus directory> tests/level/fuzz_seeds

#include "ith/ecs/World.h"
#include "ith/foundation/Uuid.h"
#include "ith/level/Level.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/serialization/TextArchive.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

ITH_DECLARE_REFLECTION_MODULE(ecs)
ITH_DECLARE_REFLECTION_MODULE(level)
ITH_DECLARE_REFLECTION_MODULE(test_level)
ITH_DECLARE_REFLECTION_MODULE(test_level_editor)

namespace {

const ith::TypeRegistry& registry() {
    static const ith::TypeRegistry kRegistry = [] {
        ith::TypeRegistry result;
        ith::generated::registerReflection_ecs(result);
        ith::generated::registerReflection_level(result);
        ith::generated::registerReflection_test_level(result);
        ith::generated::registerReflection_test_level_editor(result);
        return result;
    }();
    return kRegistry;
}

// Saves `world`; a save of a loaded level can't fail, since everything in it was read from text.
std::string save(const ith::World& world, const ith::Uuid& id) {
    const auto text = ith::saveLevel(world, id);
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
