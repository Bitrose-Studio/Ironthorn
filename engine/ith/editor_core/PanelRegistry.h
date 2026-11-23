#pragma once

// ith::PanelRegistry: the editor's dockable panels (ADR-0040 rules 1 to 3).
//
//   registry.add({.id = "ith.output_log", .displayName = "Output Log", .defaultDock = ith::DockPlace::Bottom,
//                 .draw = [](std::string_view instance) { ... }});
//   registry.add({.id = "ith.viewport", .displayName = "Viewport", .maxInstances = 4});
//   registry.resolve("ith.viewport.2");   // the viewport panel, instance 2
//   registry.title("ith.viewport.2");     // "Viewport 2"
//
// A panel is named by a stable id, "ith.<name>" in lower snake case, never reused. Its instances
// are named by instance ids: a single panel's instance id is its id; a multi-instance panel's are
// "<id>.<n>", n from 1 to maxInstances. Layout documents name instances. Each panel is registered by
// the module that brings its view, which sets `draw`; editor_core never calls it, and headless
// tests leave it null. Main thread only.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ith {

// Where Open Panel docks a closed panel (ADR-0040 rule 14).
enum class DockPlace : std::uint8_t {
    Center,
    Left,
    Right,
    Bottom,
    Floating,
};

struct PanelDesc {
    // Stable, "ith.<name>" in lower snake case.
    std::string id;
    // In ADR-0008 vocabulary, e.g. "Outliner".
    std::string displayName;
    DockPlace defaultDock = DockPlace::Center;
    // 1 for a single panel; more for a multi-instance one.
    std::uint32_t maxInstances = 1;
    // Draws one instance inside its window; set by the module that brings the view.
    std::function<void(std::string_view instanceId)> draw;
};

// An instance id resolved: its panel and its number (0 for a single panel).
struct PanelInstance {
    const PanelDesc* panel = nullptr;
    std::uint32_t number = 0;
};

class PanelRegistry {
public:
    PanelRegistry() = default;
    PanelRegistry(const PanelRegistry&) = delete;
    PanelRegistry& operator=(const PanelRegistry&) = delete;
    PanelRegistry(PanelRegistry&&) = delete;
    PanelRegistry& operator=(PanelRegistry&&) = delete;
    ~PanelRegistry() = default;

    // Registers a panel. Preconditions (asserted; the panel is then not added): an id of the form
    // "ith.<name>" not registered yet, a display name and maxInstances of at least 1.
    void add(PanelDesc panel);

    // The panel with the id `id`, or null.
    [[nodiscard]] const PanelDesc* find(std::string_view id) const;

    // Every panel, in registration order (the Window menu's order).
    [[nodiscard]] std::vector<const PanelDesc*> panels() const;

    // The panel and number an instance id names, or nullopt when no registered panel has it:
    // an unknown id, a number on a single panel, a missing number on a multi-instance panel, or
    // one outside 1..maxInstances.
    [[nodiscard]] std::optional<PanelInstance> resolve(std::string_view instanceId) const;

    // Every instance id of every panel, in registration order and instance order.
    [[nodiscard]] std::vector<std::string> instanceIds() const;

    // The title an instance shows: the display name, then " <n>" for a multi-instance panel's
    // instance. Precondition: resolve(instanceId) succeeds (asserted; returns the id otherwise).
    [[nodiscard]] std::string title(std::string_view instanceId) const;

    // Changes whenever a panel is added.
    [[nodiscard]] std::uint64_t generation() const noexcept { return m_panels.size(); }

private:
    // Stable addresses: panels() hands out pointers.
    std::vector<std::unique_ptr<PanelDesc>> m_panels;
};

// The id of instance `number` of `panel`: its id when it is a single panel, "<id>.<number>"
// otherwise.
[[nodiscard]] std::string instanceIdOf(const PanelDesc& panel, std::uint32_t number);

} // namespace ith
