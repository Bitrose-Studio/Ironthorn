#include "ith/editor_core/EditorLayout.h"

#include "ith/foundation/Assert.h"
#include "ith/serialization/Error.h"
#include "ith/serialization/TextArchive.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <set>
#include <utility>

namespace ith {

namespace {

using serialization::Value;

constexpr double kMinRatio = 0.02;
constexpr double kMaxRatio = 0.98;
constexpr double kMinRectSize = 0.1;

// A tree node with owned children, for parsing and editing; layouts store the flat form.
struct Tree {
    bool split = false;
    SplitAxis axis = SplitAxis::LeftRight;
    double ratio = 0.5;
    std::unique_ptr<Tree> first;
    std::unique_ptr<Tree> second;
    std::vector<std::string> tabs;
    std::string active;
    bool central = false;
};

std::unique_ptr<Tree> stack(std::vector<std::string> tabs, bool central = false) {
    auto node = std::make_unique<Tree>();
    node->active = tabs.empty() ? std::string() : tabs.front();
    node->tabs = std::move(tabs);
    node->central = central;
    return node;
}

std::unique_ptr<Tree> splitOf(SplitAxis axis, double ratio, std::unique_ptr<Tree> first, std::unique_ptr<Tree> second) {
    auto node = std::make_unique<Tree>();
    node->split = true;
    node->axis = axis;
    node->ratio = ratio;
    node->first = std::move(first);
    node->second = std::move(second);
    return node;
}

std::unique_ptr<Tree> toTree(const LayoutTree& flat, std::uint32_t index, std::size_t depth = 0) {
    const bool valid = index < flat.nodes.size() && depth <= kMaxLayoutDepth;
    ITH_ASSERT_MSG(valid, "a layout tree's child index is out of range or the tree is too deep");
    if (!valid) {
        return stack({});
    }
    const LayoutNode& node = flat.nodes[index];
    auto tree = std::make_unique<Tree>();
    tree->split = node.split;
    if (node.split) {
        tree->axis = node.axis;
        tree->ratio = node.ratio;
        tree->first = toTree(flat, node.first, depth + 1);
        tree->second = toTree(flat, node.second, depth + 1);
    } else {
        tree->tabs = node.tabs;
        tree->active = node.active;
        tree->central = node.central;
    }
    return tree;
}

std::uint32_t flatten(const Tree& tree, LayoutTree& out) {
    const auto index = static_cast<std::uint32_t>(out.nodes.size());
    out.nodes.emplace_back();
    LayoutNode node;
    node.split = tree.split;
    if (tree.split) {
        node.axis = tree.axis;
        node.ratio = tree.ratio;
        node.first = flatten(*tree.first, out);
        node.second = flatten(*tree.second, out);
    } else {
        node.tabs = tree.tabs;
        node.active = tree.active;
        node.central = tree.central;
    }
    out.nodes[index] = std::move(node);
    return index;
}

LayoutTree flattened(const Tree& tree) {
    LayoutTree out;
    flatten(tree, out);
    return out;
}

// Calls `visit` on every tab stack in pre-order; stops when it returns true and returns that stack.
template <typename Visit>
Tree* findStack(Tree& tree, Visit&& visit) {
    if (!tree.split) {
        return visit(tree) ? &tree : nullptr;
    }
    if (Tree* found = findStack(*tree.first, visit); found != nullptr) {
        return found;
    }
    return findStack(*tree.second, visit);
}

// Removes empty stacks other than the central one; a split left with one child becomes it. Null when
// nothing is left.
std::unique_ptr<Tree> prune(std::unique_ptr<Tree> tree) {
    if (!tree->split) {
        return tree->tabs.empty() && !tree->central ? nullptr : std::move(tree);
    }
    tree->first = prune(std::move(tree->first));
    tree->second = prune(std::move(tree->second));
    if (tree->first == nullptr) {
        return std::move(tree->second);
    }
    if (tree->second == nullptr) {
        return std::move(tree->first);
    }
    return tree;
}

// The working form of a layout.
struct Working {
    std::unique_ptr<Tree> main;
    std::vector<std::pair<LayoutRect, std::unique_ptr<Tree>>> floating;
};

Working toWorking(const EditorLayout& layout) {
    Working working;
    working.main = layout.main.nodes.empty() ? stack({}, true) : toTree(layout.main, 0);
    for (const FloatingGroup& group : layout.floating) {
        working.floating.emplace_back(group.rect, group.tree.nodes.empty() ? stack({}) : toTree(group.tree, 0));
    }
    return working;
}

// Prunes every tree; the main tree keeps its central stack, and emptied floating groups go.
void pruneAll(Working& working) {
    working.main = prune(std::move(working.main));
    ITH_ASSERT_MSG(working.main != nullptr, "the central stack keeps the main tree");
    if (working.main == nullptr) {
        working.main = stack({}, true);
    }
    std::erase_if(working.floating, [](auto& group) {
        group.second = prune(std::move(group.second));
        return group.second == nullptr;
    });
}

void fromWorking(const Working& working, EditorLayout& layout) {
    layout.main = flattened(*working.main);
    layout.floating.clear();
    for (const auto& [rect, tree] : working.floating) {
        layout.floating.push_back({.rect = rect, .tree = flattened(*tree)});
    }
}

// The normalization of ADR-0040 rule 9.
class Normalizer {
public:
    Normalizer(const PanelRegistry& panels, std::vector<Error>* warnings) : m_panels(&panels), m_warnings(warnings) {}

    void run(Working& working) {
        settleCentral(*working.main);
        filter(*working.main);
        for (auto& [rect, tree] : working.floating) {
            clearCentral(*tree);
            filter(*tree);
        }
        pruneAll(working);
        fixValues(*working.main);
        for (auto& [rect, tree] : working.floating) {
            fixValues(*tree);
            rect = clampRect(rect);
        }
    }

private:
    void warn(std::string code, std::string message) {
        if (m_warnings != nullptr) {
            m_warnings->push_back(Error{.code = std::move(code), .message = std::move(message)});
        }
    }

    // Exactly one central stack in the main tree: the first marked one, else the first stack.
    static void settleCentral(Tree& main) {
        bool found = false;
        findStack(main, [&found](Tree& node) {
            if (node.central) {
                node.central = !found;
                found = true;
            }
            return false;
        });
        if (!found) {
            findStack(main, [](Tree& node) {
                node.central = true;
                return true;
            });
        }
    }

    static void clearCentral(Tree& tree) {
        findStack(tree, [](Tree& node) {
            node.central = false;
            return false;
        });
    }

    // Drops unknown and repeated instances from every stack.
    void filter(Tree& tree) {
        findStack(tree, [this](Tree& node) {
            std::erase_if(node.tabs, [this](const std::string& id) {
                if (!m_panels->resolve(id).has_value()) {
                    if (m_unknown.insert(id).second) {
                        warn("editor.layout.unknown_panel",
                             "the layout names the unknown panel '" + id + "'; it is dropped");
                    }
                    return true;
                }
                if (!m_seen.insert(id).second) {
                    warn("editor.layout.duplicate_panel",
                         "the layout names the panel '" + id + "' twice; it stays at its first place");
                    return true;
                }
                return false;
            });
            return false;
        });
    }

    void fixValues(Tree& tree) {
        if (!tree.split) {
            if (tree.tabs.empty()) {
                tree.active.clear();
            } else if (std::ranges::find(tree.tabs, tree.active) == tree.tabs.end()) {
                tree.active = tree.tabs.front();
            }
            return;
        }
        if (!std::isfinite(tree.ratio) || tree.ratio < kMinRatio || tree.ratio > kMaxRatio) {
            warn("editor.layout.clamped", "a split ratio outside [0.02, 0.98] is clamped");
            tree.ratio = std::isfinite(tree.ratio) ? std::clamp(tree.ratio, kMinRatio, kMaxRatio) : 0.5;
        }
        tree.ratio = roundLayoutFraction(tree.ratio);
        fixValues(*tree.first);
        fixValues(*tree.second);
    }

    LayoutRect clampRect(LayoutRect rect) {
        const LayoutRect original = rect;
        const auto finiteOr = [](double value, double fallback) { return std::isfinite(value) ? value : fallback; };
        rect.width = std::clamp(finiteOr(rect.width, 0.4), kMinRectSize, 1.0);
        rect.height = std::clamp(finiteOr(rect.height, 0.5), kMinRectSize, 1.0);
        rect.x = std::clamp(finiteOr(rect.x, 0.0), 0.0, 1.0 - rect.width);
        rect.y = std::clamp(finiteOr(rect.y, 0.0), 0.0, 1.0 - rect.height);
        if (!(rect == original)) {
            warn("editor.layout.clamped", "a floating rectangle outside the dock space is clamped");
        }
        rect.x = roundLayoutFraction(rect.x);
        rect.y = roundLayoutFraction(rect.y);
        rect.width = roundLayoutFraction(rect.width);
        rect.height = roundLayoutFraction(rect.height);
        return rect;
    }

    const PanelRegistry* m_panels;
    std::vector<Error>* m_warnings;
    std::set<std::string, std::less<>> m_seen;
    std::set<std::string, std::less<>> m_unknown;
};

// Reads the document's data into the working form, checking its shape and limits.
class Parser {
public:
    Parser(std::string_view source, std::vector<Error>* warnings) : m_source(source), m_warnings(warnings) {}

    Expected<Working> run(const Value& data) {
        if (data.kind() != Value::Kind::Object) {
            return invalid("$", "the data is not an object");
        }
        Working working;
        for (const Value::Member& member : data.members()) {
            if (member.key != "root" && member.key != "floating" && member.key != "editors") {
                unknownKey("$." + member.key);
            }
        }
        const Value* root = data.find("root");
        if (root == nullptr) {
            return invalid("$", "\"root\" is missing");
        }
        Expected<std::unique_ptr<Tree>> main = node(*root, "$.root", 1);
        if (!main) {
            return makeUnexpected(std::move(main).error());
        }
        working.main = std::move(*main);
        if (const Value* floating = data.find("floating"); floating != nullptr) {
            if (floating->kind() != Value::Kind::Array) {
                return invalid("$.floating", "not an array");
            }
            for (std::size_t index = 0; index < floating->elements().size(); ++index) {
                Expected<std::pair<LayoutRect, std::unique_ptr<Tree>>> group =
                    floatingGroup(floating->elements()[index], "$.floating[" + std::to_string(index) + "]");
                if (!group) {
                    return makeUnexpected(std::move(group).error());
                }
                working.floating.push_back(std::move(*group));
            }
        }
        if (const Value* editors = data.find("editors"); editors != nullptr && editors->kind() != Value::Kind::Object) {
            return invalid("$.editors", "not an object");
        }
        return working;
    }

private:
    Unexpected<Error> invalid(const std::string& path, std::string_view message) const {
        return makeError("editor.layout.invalid",
                         std::string(m_source) + ": " + std::string(message) + " (at " + path + ")");
    }

    void unknownKey(const std::string& path) {
        if (m_warnings != nullptr) {
            m_warnings->push_back(
                Error{.code = "editor.layout.unknown_key",
                      .message = std::string(m_source) + ": " + path + " is not a layout key; it is ignored"});
        }
    }

    Expected<double> number(const Value& object, std::string_view key, const std::string& path) const {
        const Value* value = object.find(key);
        const std::optional<double> number = value != nullptr ? value->toNumber<double>() : std::nullopt;
        if (!number.has_value() || !std::isfinite(*number)) {
            return invalid(path + "." + std::string(key), "not a number");
        }
        return *number;
    }

    Expected<std::unique_ptr<Tree>> node(const Value& value, const std::string& path, std::size_t depth) {
        if (depth > kMaxLayoutDepth) {
            return invalid(path, "the tree is nested too deep");
        }
        if (++m_nodes > kMaxLayoutNodes) {
            return invalid(path, "the layout has too many nodes");
        }
        if (value.kind() != Value::Kind::Object) {
            return invalid(path, "a node is not an object");
        }
        if (const Value* split = value.find("split"); split != nullptr) {
            return splitNode(value, *split, path, depth);
        }
        if (value.find("tabs") != nullptr) {
            return stackNode(value, path);
        }
        return invalid(path, "a node is neither a split nor a tab stack");
    }

    Expected<std::unique_ptr<Tree>> splitNode(const Value& value, const Value& split, const std::string& path,
                                              std::size_t depth) {
        auto tree = std::make_unique<Tree>();
        tree->split = true;
        if (split.kind() == Value::Kind::String && split.asString() == "left_right") {
            tree->axis = SplitAxis::LeftRight;
        } else if (split.kind() == Value::Kind::String && split.asString() == "top_bottom") {
            tree->axis = SplitAxis::TopBottom;
        } else {
            return invalid(path + ".split", R"(not "left_right" or "top_bottom")");
        }
        Expected<double> ratio = number(value, "ratio", path);
        if (!ratio) {
            return makeUnexpected(std::move(ratio).error());
        }
        tree->ratio = *ratio;
        for (const auto& [key, child] : {std::pair{"first", &tree->first}, std::pair{"second", &tree->second}}) {
            const Value* childValue = value.find(key);
            if (childValue == nullptr) {
                return invalid(path, "a split misses \"" + std::string(key) + "\"");
            }
            Expected<std::unique_ptr<Tree>> parsed = node(*childValue, path + "." + key, depth + 1);
            if (!parsed) {
                return makeUnexpected(std::move(parsed).error());
            }
            *child = std::move(*parsed);
        }
        for (const Value::Member& member : value.members()) {
            if (member.key != "split" && member.key != "ratio" && member.key != "first" && member.key != "second") {
                unknownKey(path + "." + member.key);
            }
        }
        return tree;
    }

    Expected<std::unique_ptr<Tree>> stackNode(const Value& value, const std::string& path) {
        auto tree = std::make_unique<Tree>();
        const Value& tabs = *value.find("tabs");
        if (tabs.kind() != Value::Kind::Array) {
            return invalid(path + ".tabs", "not an array");
        }
        for (std::size_t index = 0; index < tabs.elements().size(); ++index) {
            const Value& tab = tabs.elements()[index];
            if (tab.kind() != Value::Kind::String) {
                return invalid(path + ".tabs[" + std::to_string(index) + "]", "not a string");
            }
            tree->tabs.push_back(tab.asString());
        }
        if (const Value* active = value.find("active"); active != nullptr) {
            if (active->kind() != Value::Kind::String) {
                return invalid(path + ".active", "not a string");
            }
            tree->active = active->asString();
        }
        if (const Value* central = value.find("central"); central != nullptr) {
            if (central->kind() != Value::Kind::Bool) {
                return invalid(path + ".central", "not a boolean");
            }
            tree->central = central->asBool();
        }
        for (const Value::Member& member : value.members()) {
            if (member.key != "tabs" && member.key != "active" && member.key != "central") {
                unknownKey(path + "." + member.key);
            }
        }
        return tree;
    }

    Expected<std::pair<LayoutRect, std::unique_ptr<Tree>>> floatingGroup(const Value& value, const std::string& path) {
        if (value.kind() != Value::Kind::Object) {
            return invalid(path, "a floating group is not an object");
        }
        const Value* rect = value.find("rect");
        if (rect == nullptr || rect->kind() != Value::Kind::Object) {
            return invalid(path + ".rect", "missing or not an object");
        }
        LayoutRect bounds;
        for (const auto& [key, field] : {std::pair{"x", &bounds.x}, std::pair{"y", &bounds.y},
                                         std::pair{"width", &bounds.width}, std::pair{"height", &bounds.height}}) {
            Expected<double> read = number(*rect, key, path + ".rect");
            if (!read) {
                return makeUnexpected(std::move(read).error());
            }
            *field = *read;
        }
        const Value* tree = value.find("node");
        if (tree == nullptr) {
            return invalid(path, "a floating group misses \"node\"");
        }
        Expected<std::unique_ptr<Tree>> parsed = node(*tree, path + ".node", 1);
        if (!parsed) {
            return makeUnexpected(std::move(parsed).error());
        }
        for (const Value::Member& member : value.members()) {
            if (member.key != "rect" && member.key != "node") {
                unknownKey(path + "." + member.key);
            }
        }
        return std::pair{bounds, std::move(*parsed)};
    }

    std::string_view m_source;
    std::vector<Error>* m_warnings;
    std::size_t m_nodes = 0;
};

Value nodeToValue(const LayoutTree& tree, std::uint32_t index) {
    const LayoutNode& node = tree.nodes[index];
    Value value = Value::object();
    if (node.split) {
        value.set("split", Value::string(node.axis == SplitAxis::LeftRight ? "left_right" : "top_bottom"));
        value.set("ratio", Value::real(node.ratio));
        value.set("first", nodeToValue(tree, node.first));
        value.set("second", nodeToValue(tree, node.second));
        return value;
    }
    Value tabs = Value::array();
    for (const std::string& tab : node.tabs) {
        tabs.elements().push_back(Value::string(tab));
    }
    value.set("tabs", std::move(tabs));
    if (!node.active.empty()) {
        value.set("active", Value::string(node.active));
    }
    if (node.central) {
        value.set("central", Value::boolean(true));
    }
    return value;
}

} // namespace

double roundLayoutFraction(double value) {
    return std::round(value * 10000.0) / 10000.0;
}

void normalizeLayout(EditorLayout& layout, const PanelRegistry& panels, std::vector<Error>* warnings) {
    Working working = toWorking(layout);
    Normalizer(panels, warnings).run(working);
    fromWorking(working, layout);
}

Expected<EditorLayout> layoutFromValue(const Value& data, std::string_view source, const PanelRegistry& panels,
                                       std::vector<Error>* warnings) {
    Expected<Working> working = Parser(source, warnings).run(data);
    if (!working) {
        return makeUnexpected(std::move(working).error());
    }
    Normalizer(panels, warnings).run(*working);
    EditorLayout layout;
    fromWorking(*working, layout);
    if (const Value* editors = data.find("editors"); editors != nullptr) {
        layout.editors = *editors;
    }
    return layout;
}

Expected<EditorLayout> readLayout(std::string_view text, std::string_view source, const PanelRegistry& panels,
                                  std::vector<Error>* warnings) {
    std::vector<serialization::Error> envelopeWarnings;
    Expected<Value, serialization::Error> data = serialization::readDocument(
        text, kEditorLayoutFormat, {.source = source, .warnings = &envelopeWarnings, .maxSize = kMaxLayoutFileSize});
    if (!data) {
        return makeError("editor.layout.invalid", data.error().toString());
    }
    if (warnings != nullptr) {
        for (const serialization::Error& warning : envelopeWarnings) {
            warnings->push_back(Error{.code = warning.code, .message = warning.toString()});
        }
    }
    return layoutFromValue(*data, source, panels, warnings);
}

Value layoutToValue(const EditorLayout& layout) {
    Value data = Value::object();
    data.set("root", layout.main.nodes.empty() ? Value::object() : nodeToValue(layout.main, 0));
    Value floating = Value::array();
    for (const FloatingGroup& group : layout.floating) {
        Value rect = Value::object();
        rect.set("x", Value::real(group.rect.x));
        rect.set("y", Value::real(group.rect.y));
        rect.set("width", Value::real(group.rect.width));
        rect.set("height", Value::real(group.rect.height));
        Value entry = Value::object();
        entry.set("rect", std::move(rect));
        entry.set("node", group.tree.nodes.empty() ? Value::object() : nodeToValue(group.tree, 0));
        floating.elements().push_back(std::move(entry));
    }
    data.set("floating", std::move(floating));
    data.set("editors", layout.editors);
    return data;
}

Expected<std::string> writeLayout(const EditorLayout& layout) {
    Expected<std::string, serialization::Error> text =
        serialization::writeDocument(kEditorLayoutFormat, layoutToValue(layout));
    if (!text) {
        return makeError(text.error().code, text.error().toString());
    }
    return std::move(*text);
}

std::vector<std::string> openPanels(const EditorLayout& layout) {
    std::vector<std::string> open;
    const auto collect = [&open](const LayoutTree& tree) {
        // Pre-order storage: stacks in index order are the stacks in pre-order.
        for (const LayoutNode& node : tree.nodes) {
            if (!node.split) {
                open.insert(open.end(), node.tabs.begin(), node.tabs.end());
            }
        }
    };
    collect(layout.main);
    for (const FloatingGroup& group : layout.floating) {
        collect(group.tree);
    }
    return open;
}

bool isPanelOpen(const EditorLayout& layout, std::string_view instanceId) {
    const std::vector<std::string> open = openPanels(layout);
    return std::ranges::find(open, instanceId) != open.end();
}

void openPanel(EditorLayout& layout, std::string_view instanceId, const PanelRegistry& panels) {
    const std::optional<PanelInstance> instance = panels.resolve(instanceId);
    ITH_ASSERT_MSG(instance.has_value(), "openPanel: not a registered panel instance");
    ITH_ASSERT_MSG(!isPanelOpen(layout, instanceId), "openPanel: the panel is open already");
    if (!instance || isPanelOpen(layout, instanceId)) {
        return;
    }
    const DockPlace place = instance->panel->defaultDock;
    const std::string id(instanceId);
    Working working = toWorking(layout);

    const auto samePlace = [&panels, place](Tree& node) {
        return std::ranges::any_of(node.tabs, [&panels, place](const std::string& tab) {
            const std::optional<PanelInstance> other = panels.resolve(tab);
            return other.has_value() && other->panel->defaultDock == place;
        });
    };
    Tree* target = findStack(*working.main, samePlace);
    for (const auto& group : working.floating) {
        if (target == nullptr) {
            target = findStack(*group.second, samePlace);
        }
    }
    if (target == nullptr && place == DockPlace::Center) {
        target = findStack(*working.main, [](Tree& node) { return node.central; });
    }
    if (target != nullptr) {
        target->tabs.push_back(id);
        target->active = id;
    } else if (place == DockPlace::Floating) {
        working.floating.emplace_back(LayoutRect{.x = 0.3, .y = 0.25, .width = 0.4, .height = 0.5}, stack({id}));
    } else if (place == DockPlace::Left) {
        working.main = splitOf(SplitAxis::LeftRight, 0.2, stack({id}), std::move(working.main));
    } else if (place == DockPlace::Right) {
        working.main = splitOf(SplitAxis::LeftRight, 0.78, std::move(working.main), stack({id}));
    } else {
        working.main = splitOf(SplitAxis::TopBottom, 0.7, std::move(working.main), stack({id}));
    }
    fromWorking(working, layout);
}

bool closePanel(EditorLayout& layout, std::string_view instanceId) {
    if (!isPanelOpen(layout, instanceId)) {
        return false;
    }
    Working working = toWorking(layout);
    const auto remove = [instanceId](Tree& node) {
        const auto found = std::ranges::find(node.tabs, instanceId);
        if (found == node.tabs.end()) {
            return false;
        }
        node.tabs.erase(found);
        if (node.active == instanceId) {
            node.active = node.tabs.empty() ? std::string() : node.tabs.front();
        }
        return true;
    };
    if (findStack(*working.main, remove) == nullptr) {
        for (const auto& group : working.floating) {
            if (findStack(*group.second, remove) != nullptr) {
                break;
            }
        }
    }
    pruneAll(working);
    fromWorking(working, layout);
    return true;
}

bool activatePanel(EditorLayout& layout, std::string_view instanceId) {
    const auto activate = [instanceId](LayoutTree& tree) {
        for (LayoutNode& node : tree.nodes) {
            if (!node.split && std::ranges::find(node.tabs, instanceId) != node.tabs.end()) {
                node.active = std::string(instanceId);
                return true;
            }
        }
        return false;
    };
    if (activate(layout.main)) {
        return true;
    }
    return std::ranges::any_of(layout.floating, [&activate](FloatingGroup& group) { return activate(group.tree); });
}

EditorLayout builtInDefaultLayout() {
    Working working;
    working.main = splitOf(SplitAxis::LeftRight, 0.78,
                           splitOf(SplitAxis::TopBottom, 0.7, stack({"ith.viewport.1"}, true),
                                   stack({"ith.content_browser", "ith.output_log"})),
                           splitOf(SplitAxis::TopBottom, 0.4, stack({"ith.outliner"}), stack({"ith.details"})));
    EditorLayout layout;
    fromWorking(working, layout);
    return layout;
}

} // namespace ith
