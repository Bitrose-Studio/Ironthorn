#pragma once

// ith::EditorPreferences: the editor's own settings, the "preferences" section of Editor.json in
// the layered configuration (ADR-0035 rules 16 and 21). The field initializers are the engine
// defaults; a layer's file holds only what differs.
//
//   ith::EditorPreferences preferences = config.get(ith::kEditorPreferences);

#include "ith/config/Config.h"
#include "ith/reflection/Annotations.h"

#include <cstdint>

namespace ith {

struct EditorPreferences {
    ITH_TYPE(Struct)
    // The frames per second the editor draws while nothing changes and one of its windows has
    // focus, and while none has (ADR-0029 rule 12). Activity (input, edits, finished jobs) draws at
    // once, whatever these say. At least 1.
    ITH_FIELD(Min = 1, Max = 240)
    std::int32_t idleFramesPerSecond = 10;
    ITH_FIELD(Min = 1, Max = 240)
    std::int32_t idleFramesPerSecondUnfocused = 2;
};

inline constexpr ConfigSection<EditorPreferences> kEditorPreferences{"Editor", "preferences"};

} // namespace ith
