#include "ith/editor_ui/DockLayout.h"

#include "ith/editor_core/EditorLayout.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/LayoutSession.h"
#include "ith/editor_core/PanelRegistry.h"
#include "ith/serialization/Value.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ith {

namespace {

constexpr std::string_view kIdSeparator = "###";

std::string windowName(const PanelRegistry& panels, std::string_view instance) {
    return panels.title(instance) + std::string(kIdSeparator) + std::string(instance);
}

// The instance id of a panel window ("<title>###<instance id>"), or empty for other windows.
std::string_view instanceOf(const ImGuiWindow& window) {
    const std::string_view name = window.Name;
    const std::size_t at = name.find(kIdSeparator);
    return at == std::string_view::npos ? std::string_view() : name.substr(at + kIdSeparator.size());
}

bool containsCentral(const LayoutTree& tree, std::uint32_t index) {
    const LayoutNode& node = tree.nodes[index];
    if (!node.split) {
        return node.central;
    }
    return containsCentral(tree, node.first) || containsCentral(tree, node.second);
}

struct Builder {
    const PanelRegistry* panels;
    // The stacks built and their active windows, selected once ImGui has docked the windows.
    std::vector<std::pair<ImGuiID, std::string>> actives;

    void node(const LayoutTree& tree, std::uint32_t index, ImGuiID id) {
        const LayoutNode& node = tree.nodes[index];
        if (!node.split) {
            for (const std::string& tab : node.tabs) {
                ImGui::DockBuilderDockWindow(windowName(*panels, tab).c_str(), id);
            }
            if (!node.active.empty()) {
                actives.emplace_back(id, windowName(*panels, node.active));
            }
            return;
        }
        // ImGui keeps a node's content and central flag in the child opposite the split direction, so
        // the split goes away from the child that holds the central stack.
        const bool centralFirst = containsCentral(tree, node.first);
        const bool leftRight = node.axis == SplitAxis::LeftRight;
        const ImGuiDir towardsFirst = leftRight ? ImGuiDir_Left : ImGuiDir_Up;
        const ImGuiDir towardsSecond = leftRight ? ImGuiDir_Right : ImGuiDir_Down;
        ImGuiID atDir = 0;
        ImGuiID opposite = 0;
        const auto ratio = static_cast<float>(node.ratio);
        ImGui::DockBuilderSplitNode(id, centralFirst ? towardsSecond : towardsFirst,
                                    centralFirst ? 1.0f - ratio : ratio, &atDir, &opposite);
        this->node(tree, node.first, centralFirst ? opposite : atDir);
        this->node(tree, node.second, centralFirst ? atDir : opposite);
    }
};

// Reads a dock node's subtree into `tree` in pre-order; `open` lists the instances the layout
// holds open, the only windows read back.
std::uint32_t readNode(const ImGuiDockNode& dock, LayoutTree& tree, const std::vector<std::string>& open) {
    const auto index = static_cast<std::uint32_t>(tree.nodes.size());
    tree.nodes.emplace_back();
    LayoutNode node;
    if (dock.IsSplitNode()) {
        const int axis = dock.SplitAxis == ImGuiAxis_X ? 0 : 1;
        const float first = dock.ChildNodes[0]->Size[axis];
        const float total = first + dock.ChildNodes[1]->Size[axis];
        node.split = true;
        node.axis = axis == 0 ? SplitAxis::LeftRight : SplitAxis::TopBottom;
        node.ratio = total > 0.0f ? static_cast<double>(first / total) : 0.5;
        node.first = readNode(*dock.ChildNodes[0], tree, open);
        node.second = readNode(*dock.ChildNodes[1], tree, open);
        tree.nodes[index] = std::move(node);
        return index;
    }
    const auto add = [&node, &open](const ImGuiWindow* window, ImGuiID selected) {
        if (window == nullptr) {
            return;
        }
        const std::string_view instance = instanceOf(*window);
        if (std::ranges::find(open, instance) == open.end() ||
            std::ranges::find(node.tabs, instance) != node.tabs.end()) {
            return;
        }
        node.tabs.emplace_back(instance);
        if (window->TabId == selected) {
            node.active = std::string(instance);
        }
    };
    if (dock.TabBar != nullptr) {
        for (const ImGuiTabItem& tab : dock.TabBar->Tabs) {
            add(tab.Window, dock.SelectedTabId);
        }
    }
    for (const ImGuiWindow* window : dock.Windows) {
        add(window, dock.SelectedTabId);
    }
    node.central = dock.IsCentralNode();
    tree.nodes[index] = std::move(node);
    return index;
}

} // namespace

DockLayout::DockLayout(EditorSession& session) : m_session(&session) {
    session.layouts().setReadBack([this] { readBack(); });
}

DockLayout::~DockLayout() {
    m_session->layouts().setReadBack(nullptr);
}

void DockLayout::build() {
    const LayoutSession& layouts = m_session->layouts();
    const EditorLayout& layout = layouts.current();
    const PanelRegistry& panels = m_session->panels();

    // Floating nodes of panel windows go with the old tree.
    for (const std::string& instance : panels.instanceIds()) {
        const ImGuiWindow* window = ImGui::FindWindowByName(windowName(panels, instance).c_str());
        if (window != nullptr && window->DockNode != nullptr) {
            const ImGuiDockNode* root = ImGui::DockNodeGetRootNode(window->DockNode);
            if (root->ID != m_dockSpace) {
                ImGui::DockBuilderRemoveNode(root->ID);
            }
        }
    }
    ImGui::DockBuilderRemoveNode(m_dockSpace);
    ImGui::DockBuilderAddNode(m_dockSpace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodePos(m_dockSpace, {m_x, m_y});
    ImGui::DockBuilderSetNodeSize(m_dockSpace, {m_width, m_height});

    Builder builder{.panels = &panels, .actives = {}};
    builder.node(layout.main, 0, m_dockSpace);
    for (std::size_t group = 0; group < layout.floating.size(); ++group) {
        const FloatingGroup& floating = layout.floating[group];
        const ImGuiID id = ImHashStr(("ith.floating." + std::to_string(group)).c_str());
        ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_None);
        ImGui::DockBuilderSetNodePos(id, {m_x + static_cast<float>(floating.rect.x) * m_width,
                                          m_y + static_cast<float>(floating.rect.y) * m_height});
        ImGui::DockBuilderSetNodeSize(id, {static_cast<float>(floating.rect.width) * m_width,
                                           static_cast<float>(floating.rect.height) * m_height});
        builder.node(floating.tree, 0, id);
        ImGui::DockBuilderFinish(id);
    }
    ImGui::DockBuilderFinish(m_dockSpace);
    m_actives.clear();
    for (auto& [id, window] : builder.actives) {
        m_actives.push_back(std::move(window));
    }
    m_applied = layouts.generation();
    m_built = true;
    m_settleFrames = 2;
    ++m_rebuilds;
}

void DockLayout::selectActiveTabs() {
    for (const std::string& name : m_actives) {
        const ImGuiWindow* window = ImGui::FindWindowByName(name.c_str());
        if (window != nullptr && window->DockNode != nullptr && window->DockNode->TabBar != nullptr) {
            window->DockNode->TabBar->NextSelectedTabId = window->TabId;
        }
    }
}

void DockLayout::readBack() {
    // Before the first build, or while ImGui hasn't laid out the newest model, the model is right.
    if (!m_built || m_settleFrames > 0 || m_applied != m_session->layouts().generation()) {
        return;
    }
    const ImGuiDockNode* root = ImGui::DockBuilderGetNode(m_dockSpace);
    if (root == nullptr) {
        return;
    }
    const PanelRegistry& panels = m_session->panels();
    const EditorLayout& current = m_session->layouts().current();
    const std::vector<std::string> open = openPanels(current);
    EditorLayout layout;
    layout.main.nodes.clear();
    readNode(*root, layout.main, open);
    layout.editors = current.editors;

    // Floating groups: the floating dock nodes holding panel windows, and undocked panel windows.
    std::vector<const ImGuiDockNode*> seen;
    const auto fraction = [](float value, float origin, float extent) {
        return extent > 0.0f ? static_cast<double>((value - origin) / extent) : 0.0;
    };
    for (const std::string& instance : open) {
        const ImGuiWindow* window = ImGui::FindWindowByName(windowName(panels, instance).c_str());
        if (window == nullptr) {
            continue;
        }
        FloatingGroup group;
        if (window->DockNode != nullptr) {
            const ImGuiDockNode* floating = ImGui::DockNodeGetRootNode(window->DockNode);
            if (floating->ID == m_dockSpace || std::ranges::find(seen, floating) != seen.end()) {
                continue;
            }
            seen.push_back(floating);
            readNode(*floating, group.tree, open);
            group.rect = {.x = fraction(floating->Pos.x, m_x, m_width),
                          .y = fraction(floating->Pos.y, m_y, m_height),
                          .width = fraction(floating->Size.x, 0.0f, m_width),
                          .height = fraction(floating->Size.y, 0.0f, m_height)};
        } else {
            group.tree.nodes.push_back(LayoutNode{.tabs = {instance}, .active = instance});
            group.rect = {.x = fraction(window->Pos.x, m_x, m_width),
                          .y = fraction(window->Pos.y, m_y, m_height),
                          .width = fraction(window->Size.x, 0.0f, m_width),
                          .height = fraction(window->Size.y, 0.0f, m_height)};
        }
        layout.floating.push_back(std::move(group));
    }
    m_session->layouts().observe(std::move(layout));
}

void DockLayout::draw(float bottomReserved) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 size{viewport->WorkSize.x, viewport->WorkSize.y - bottomReserved};
    if (size.x <= 0.0f || size.y <= 0.0f) {
        return;
    }
    // A new display scale: keep what is on screen, then rebuild it at the new sizes.
    const bool rescale = m_rescale;
    if (rescale) {
        m_rescale = false;
        readBack();
    }
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowViewport(viewport->ID);
    // NOLINTBEGIN(bugprone-signed-bitwise): ImGui's flags are int enums
    constexpr ImGuiWindowFlags kHostFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoBackground;
    // NOLINTEND(bugprone-signed-bitwise)
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("Dock space###ith.dockspace_host", nullptr, kHostFlags);
    ImGui::PopStyleVar(3);
    m_dockSpace = ImGui::GetID(kDockSpaceName);
    m_x = viewport->WorkPos.x;
    m_y = viewport->WorkPos.y;
    m_width = size.x;
    m_height = size.y;
    const bool rebuild = rescale || !m_built || m_applied != m_session->layouts().generation();
    if (rebuild) {
        build();
    }
    ImGui::DockSpace(m_dockSpace, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();

    // The panels the layout holds open.
    const PanelRegistry& panels = m_session->panels();
    std::vector<std::string> closed;
    for (const std::string& instance : openPanels(m_session->layouts().current())) {
        const std::optional<PanelInstance> resolved = panels.resolve(instance);
        if (!resolved) {
            continue;
        }
        bool keepOpen = true;
        const std::string name = windowName(panels, instance);
        if (ImGui::Begin(name.c_str(), &keepOpen) && resolved->panel->draw) {
            resolved->panel->draw(instance);
        }
        ImGui::End();
        if (!keepOpen) {
            closed.push_back(instance);
        }
    }
    if (const std::optional<std::string> focus = m_session->layouts().takeFocusRequest(); focus.has_value()) {
        if (panels.resolve(*focus).has_value()) {
            ImGui::SetWindowFocus(windowName(panels, *focus).c_str());
        }
    }

    // After a rebuild: ImGui docks the windows as they begin, and each new tab bar selects its last
    // tab, so the active tabs are selected on the next frame and the tree is read back after that.
    // Otherwise read back when ImGui marks its settings dirty: the user moved something.
    if (!rebuild && m_settleFrames > 0) {
        if (m_settleFrames == 2) {
            selectActiveTabs();
        }
        if (--m_settleFrames == 0) {
            readBack();
        }
    }
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantSaveIniSettings) {
        io.WantSaveIniSettings = false;
        readBack();
    }
    for (const std::string& instance : closed) {
        serialization::Value args = serialization::Value::object();
        args.set("panel", serialization::Value::string(instance));
        static_cast<void>(m_session->execute("ith.window.close_panel", args));
    }
}

} // namespace ith
