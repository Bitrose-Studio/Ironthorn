#pragma once

// Reflected settings for the config tests: a section with plain fields, a list, a nested struct
// and a renamed field. Generated into the test library ith_test_config (module "test_config").

#include "ith/reflection/Annotations.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ith::test::config {

struct WindowSettings {
    ITH_TYPE(Struct)
    ITH_FIELD()
    std::int32_t width = 1280;
    ITH_FIELD()
    std::int32_t height = 720;
};

struct Preferences {
    ITH_TYPE(Struct)
    ITH_FIELD()
    std::int32_t idleFramesPerSecond = 10;
    ITH_FIELD(FormerName = "backgroundFramesPerSecond")
    std::int32_t unfocusedFramesPerSecond = 2;
    ITH_FIELD()
    std::string theme = "dark";
    ITH_FIELD()
    std::vector<std::string> recent;
    ITH_FIELD()
    WindowSettings window;
    ITH_FIELD()
    std::optional<std::int32_t> limit;
};

} // namespace ith::test::config
