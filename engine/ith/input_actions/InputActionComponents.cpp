#include "ith/input_actions/InputActionComponents.h"

#include <bit>
#include <cstddef>

namespace ith {

std::size_t phaseIndex(ActionPhase phase) noexcept {
    const auto bits = static_cast<unsigned>(phase);
    if (!std::has_single_bit(bits)) {
        return kActionPhaseCount;
    }
    const auto index = static_cast<std::size_t>(std::countr_zero(bits));
    return index < kActionPhaseCount ? index : kActionPhaseCount;
}

const ActionState* ActionStates::find(AssetId action) const noexcept {
    for (const ActionState& entry : actions) {
        if (entry.action == action.uuid()) {
            return &entry;
        }
    }
    return nullptr;
}

ActionValue ActionStates::value(AssetId action) const noexcept {
    const ActionState* entry = find(action);
    return entry != nullptr ? entry->value : ActionValue{};
}

TriggerState ActionStates::state(AssetId action) const noexcept {
    const ActionState* entry = find(action);
    return entry != nullptr ? entry->state : TriggerState::None;
}

bool ActionStates::has(AssetId action, ActionPhase phase) const noexcept {
    const ActionState* entry = find(action);
    return entry != nullptr && (entry->phases & phase) != ActionPhase::None;
}

} // namespace ith
