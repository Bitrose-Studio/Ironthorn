#pragma once

// ith::EditorLayout: where the editor's panels are, without any UI (ADR-0040 rules 4 to 9), and its
// document, "ith.editor_layout" version 1.
//
//   std::vector<ith::Error> warnings;
//   ith::Expected<ith::EditorLayout> layout = ith::readLayout(text, "Layouts/Mine.json", panels, &warnings);
//   ith::openPanel(*layout, "ith.output_log", panels);
//   ith::Expected<std::string> saved = ith::writeLayout(*layout);
//
// A layout is a main tree that fills the dock space, floating groups inside the main window, and
// the sub-layouts of asset-editor kinds (`editors`, kept as JSON until P3 defines them). A tree's
// nodes are splits (two children side by side or one above the other, with the first child's share)
// and tab stacks (instance ids in tab order, the active one, and in the main tree exactly one
// central stack, which may be empty and stays when emptied). The document:
//
//   {"format": "ith.editor_layout", "version": 1, "data": {
//       "root": {"split": "left_right", "ratio": 0.78,
//                "first": {"tabs": [], "central": true},
//                "second": {"tabs": ["ith.outliner"], "active": "ith.outliner"}},
//       "floating": [{"rect": {"x": 0.3, "y": 0.25, "width": 0.4, "height": 0.5},
//                     "node": {"tabs": ["ith.output_log"], "active": "ith.output_log"}}],
//       "editors": {}}}
//
// Every layout these functions return is normalized: it names only registered panel instances, each
// once; emptied stacks other than the central one are gone; ratios are within [0.02, 0.98] and
// rectangles inside [0, 1] with at least 0.1 x 0.1, both rounded to 4 decimals; nodes are in
// pre-order. Two normalized layouts that look the same compare equal.

#include "ith/editor_core/PanelRegistry.h"
#include "ith/foundation/Expected.h"
#include "ith/serialization/Document.h"
#include "ith/serialization/Value.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ith {

inline constexpr serialization::DocumentFormat kEditorLayoutFormat{
    .name = "ith.editor_layout", .version = 1, .minimumVersion = 1};

// Limits of a layout document (ADR-0040 rule 5).
inline constexpr std::size_t kMaxLayoutFileSize = std::size_t{1} << 20;
inline constexpr std::size_t kMaxLayoutNodes = 256;
inline constexpr std::size_t kMaxLayoutDepth = 32;

// How a split places its children: `first` left of `second`, or above it.
enum class SplitAxis : std::uint8_t {
    LeftRight,
    TopBottom,
};

struct LayoutNode {
    // A split when true, a tab stack otherwise.
    bool split = false;

    // Split: the axis, the share of `first` in (0, 1), and the indices of the children.
    SplitAxis axis = SplitAxis::LeftRight;
    double ratio = 0.5;
    std::uint32_t first = 0;
    std::uint32_t second = 0;

    // Tab stack: instance ids in tab order, the active one (empty when there are no tabs), and
    // whether it is the central stack.
    std::vector<std::string> tabs;
    std::string active;
    bool central = false;

    friend bool operator==(const LayoutNode&, const LayoutNode&) = default;
};

// A tree of nodes in pre-order: nodes[0] is the root.
struct LayoutTree {
    std::vector<LayoutNode> nodes;

    friend bool operator==(const LayoutTree&, const LayoutTree&) = default;
};

// A rectangle as fractions of the dock space.
struct LayoutRect {
    double x = 0.0;
    double y = 0.0;
    double width = 1.0;
    double height = 1.0;

    friend bool operator==(const LayoutRect&, const LayoutRect&) = default;
};

struct FloatingGroup {
    LayoutRect rect;
    LayoutTree tree;

    friend bool operator==(const FloatingGroup&, const FloatingGroup&) = default;
};

struct EditorLayout {
    // Starts as one empty central stack.
    LayoutTree main{.nodes = {LayoutNode{.central = true}}};
    std::vector<FloatingGroup> floating;
    // The sub-layouts of asset-editor kinds, by kind id (ADR-0040 rule 7): kept as read.
    serialization::Value editors = serialization::Value::object();

    friend bool operator==(const EditorLayout&, const EditorLayout&) = default;
};

// Reads the "data" of a layout document. Fails with "editor.layout.invalid" (the message names the
// source and the JSON path) when `data` doesn't have the document's shape or exceeds the limits.
// Otherwise normalizes it against `panels` and appends to `warnings` (when not null):
// "editor.layout.unknown_panel" (once per id), "editor.layout.duplicate_panel",
// "editor.layout.clamped" and "editor.layout.unknown_key".
[[nodiscard]] Expected<EditorLayout> layoutFromValue(const serialization::Value& data, std::string_view source,
                                                     const PanelRegistry& panels, std::vector<Error>* warnings);

// Reads a layout document: the envelope (a wrong format, a newer version and unparsable text fail
// with "editor.layout.invalid" too), then layoutFromValue.
[[nodiscard]] Expected<EditorLayout> readLayout(std::string_view text, std::string_view source,
                                                const PanelRegistry& panels, std::vector<Error>* warnings);

// The "data" of `layout`'s document.
[[nodiscard]] serialization::Value layoutToValue(const EditorLayout& layout);

// `layout` as a document's text.
[[nodiscard]] Expected<std::string> writeLayout(const EditorLayout& layout);

// Normalizes a layout built in code or read back from the UI (see the header comment), with the
// warnings of layoutFromValue.
void normalizeLayout(EditorLayout& layout, const PanelRegistry& panels, std::vector<Error>* warnings);

// The open panel instances: the main tree's tabs in pre-order, then the floating groups'.
[[nodiscard]] std::vector<std::string> openPanels(const EditorLayout& layout);

[[nodiscard]] bool isPanelOpen(const EditorLayout& layout, std::string_view instanceId);

// Docks the closed instance `instanceId` at its panel's default place and makes it the active tab
// (ADR-0040 rule 14): as a tab in the first stack holding a panel with the same default place; else
// Center joins the central stack, Left, Right and Bottom split the main tree's root at that side
// (a share of 0.2, 0.22 and 0.3), and Floating makes a 0.4 x 0.5 group in the middle.
// Preconditions (asserted): the instance is registered and not open.
void openPanel(EditorLayout& layout, std::string_view instanceId, const PanelRegistry& panels);

// Closes the instance; false when it isn't open. Its stack's active tab moves to the first
// remaining one, and an emptied stack other than the central one goes.
bool closePanel(EditorLayout& layout, std::string_view instanceId);

// Makes `instanceId` the active tab of its stack; false when it isn't open.
bool activatePanel(EditorLayout& layout, std::string_view instanceId);

// The built-in default (ADR-0040 rule 18), the full P2 tree before normalization: viewport in the
// centre, Content Browser and Output Log below it, Outliner above Details on the right at full
// height. Normalizing it drops the panels not registered yet.
[[nodiscard]] EditorLayout builtInDefaultLayout();

// A ratio or rectangle fraction rounded to 4 decimals, as layouts keep them.
[[nodiscard]] double roundLayoutFraction(double value);

} // namespace ith
