#include "ith/editor_core/PanelRegistry.h"

#include "ith/foundation/Assert.h"

#include <algorithm>
#include <charconv>
#include <utility>

namespace ith {

namespace {

// "ith." followed by lower snake case.
bool isPanelId(std::string_view id) {
    constexpr std::string_view kPrefix = "ith.";
    if (!id.starts_with(kPrefix) || id.size() == kPrefix.size()) {
        return false;
    }
    return std::ranges::all_of(id.substr(kPrefix.size()),
                               [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

} // namespace

std::string instanceIdOf(const PanelDesc& panel, std::uint32_t number) {
    if (panel.maxInstances <= 1) {
        return panel.id;
    }
    return panel.id + "." + std::to_string(number);
}

void PanelRegistry::add(PanelDesc panel) {
    const bool valid = isPanelId(panel.id) && !panel.displayName.empty() && panel.maxInstances >= 1;
    ITH_ASSERT_MSG(valid, "a panel needs an id \"ith.<name>\", a display name and at least one instance");
    const bool known = find(panel.id) != nullptr;
    ITH_ASSERT_MSG(!known, "a panel with this id is registered already");
    if (!valid || known) {
        return;
    }
    m_panels.push_back(std::make_unique<PanelDesc>(std::move(panel)));
}

const PanelDesc* PanelRegistry::find(std::string_view id) const {
    for (const std::unique_ptr<PanelDesc>& panel : m_panels) {
        if (panel->id == id) {
            return panel.get();
        }
    }
    return nullptr;
}

std::vector<const PanelDesc*> PanelRegistry::panels() const {
    std::vector<const PanelDesc*> result;
    result.reserve(m_panels.size());
    for (const std::unique_ptr<PanelDesc>& panel : m_panels) {
        result.push_back(panel.get());
    }
    return result;
}

std::optional<PanelInstance> PanelRegistry::resolve(std::string_view instanceId) const {
    if (const PanelDesc* single = find(instanceId); single != nullptr) {
        if (single->maxInstances == 1) {
            return PanelInstance{.panel = single, .number = 0};
        }
        return std::nullopt;
    }
    const std::size_t dot = instanceId.rfind('.');
    if (dot == std::string_view::npos) {
        return std::nullopt;
    }
    const PanelDesc* panel = find(instanceId.substr(0, dot));
    if (panel == nullptr || panel->maxInstances == 1) {
        return std::nullopt;
    }
    const std::string_view digits = instanceId.substr(dot + 1);
    std::uint32_t number = 0;
    const std::from_chars_result parsed = std::from_chars(digits.data(), digits.data() + digits.size(), number);
    if (digits.empty() || digits.front() == '0' || parsed.ec != std::errc() ||
        parsed.ptr != digits.data() + digits.size() || number < 1 || number > panel->maxInstances) {
        return std::nullopt;
    }
    return PanelInstance{.panel = panel, .number = number};
}

std::vector<std::string> PanelRegistry::instanceIds() const {
    std::vector<std::string> ids;
    for (const std::unique_ptr<PanelDesc>& panel : m_panels) {
        if (panel->maxInstances == 1) {
            ids.push_back(panel->id);
            continue;
        }
        for (std::uint32_t number = 1; number <= panel->maxInstances; ++number) {
            ids.push_back(instanceIdOf(*panel, number));
        }
    }
    return ids;
}

std::string PanelRegistry::title(std::string_view instanceId) const {
    const std::optional<PanelInstance> instance = resolve(instanceId);
    ITH_ASSERT_MSG(instance.has_value(), "PanelRegistry::title: not a registered panel instance");
    if (!instance) {
        return std::string(instanceId);
    }
    if (instance->number == 0) {
        return instance->panel->displayName;
    }
    return instance->panel->displayName + " " + std::to_string(instance->number);
}

} // namespace ith
