#include "ith/editor_ui/ContentBrowserPanel.h"

#include "ith/editor_core/CommandRegistry.h"
#include "ith/editor_core/EditorLayout.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/PanelRegistry.h"
#include "ith/editor_ui/Icons.h"
#include "ith/foundation/Log.h"
#include "ith/level/LevelFormat.h"
#include "ith/serialization/Value.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ith {

namespace {

ITH_LOG_CATEGORY(ContentBrowser);

using serialization::Value;

// The drawer's smallest height at display scale 1, and its share of the window at most and at first.
constexpr float kDrawerMinHeight = 150.0f;
constexpr float kDrawerMaxShare = 0.8f;
constexpr float kDrawerDefaultShare = 0.4f;
// A grid tile's width and the height of its icon, at display scale 1.
constexpr float kTileWidth = 88.0f;
constexpr float kTileIconHeight = 40.0f;
// The thickness of the splitter and of the drawer's edge, at display scale 1.
constexpr float kSplitterWidth = 4.0f;

constexpr const char* kUnsavedPopup = "Unsaved Changes###ith.content_unsaved";

constexpr std::string_view kContentBrowserPanelId = "ith.content_browser";

// NOLINTBEGIN(bugprone-signed-bitwise): ImGui's flags are int enums
// A grid tile takes left and right clicks.
constexpr ImGuiButtonFlags kTileButtons = ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight;
// A list row spans every column and reports double-clicks.
constexpr ImGuiSelectableFlags kListRow = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick;
constexpr ImGuiTableFlags kListTable = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                       ImGuiTableFlags_ScrollY;
// Any popup at any level: the drawer's menus and its question.
constexpr ImGuiPopupFlags kAnyPopup = ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel;
// NOLINTEND(bugprone-signed-bitwise)

// The sort flags of the list column that sorts by `key`: the model's sort is the table's default.
ImGuiTableColumnFlags sortFlags(const ContentViewSettings& view, ContentSortKey key) {
    if (view.sortBy != key) {
        return ImGuiTableColumnFlags_None;
    }
    // NOLINTNEXTLINE(bugprone-signed-bitwise): ImGui's flags are int enums
    return ImGuiTableColumnFlags_DefaultSort |
           (view.descending ? ImGuiTableColumnFlags_PreferSortDescending : ImGuiTableColumnFlags_PreferSortAscending);
}

float displayScale() {
    return ImGui::GetStyle().FontScaleDpi;
}

UiRect rectOfItem() {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    return UiRect{.x = min.x, .y = min.y, .width = max.x - min.x, .height = max.y - min.y};
}

Value keyValue(const ContentKey& key) {
    Value ref = Value::object();
    if (key.id.isNil()) {
        ref.set("path", Value::string(key.path));
    } else {
        ref.set("id", Value::string(key.id.toString()));
    }
    return ref;
}

Value selectArgs(const ContentKey& key, std::string_view mode) {
    Value items = Value::array();
    items.elements().push_back(keyValue(key));
    Value args = Value::object();
    args.set("items", std::move(items));
    args.set("mode", Value::string(std::string(mode)));
    return args;
}

Value stringArgs(std::string_view key, std::string_view value) {
    Value args = Value::object();
    args.set(std::string(key), Value::string(std::string(value)));
    return args;
}

// The selection mode of a click with the modifiers held.
std::string_view clickMode() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyShift) {
        return io.KeyCtrl ? "add_range" : "range";
    }
    return io.KeyCtrl ? "toggle" : "replace";
}

const Icon& iconOf(const ContentItem& item) {
    if (item.kind == ContentItemKind::Folder) {
        return icons::kFolder;
    }
    if (item.kind == ContentItemKind::Problem) {
        return icons::kWarning;
    }
    switch (item.category) {
        case ContentCategory::Level:
            return icons::kMap;
        case ContentCategory::Model:
            return icons::kBox;
        case ContentCategory::Texture:
            return icons::kImage;
        case ContentCategory::Audio:
            return icons::kMusic;
        case ContentCategory::Data:
            return icons::kFileText;
        case ContentCategory::Other:
            break;
    }
    return icons::kFileQuestion;
}

std::string typeName(const ContentItem& item) {
    if (item.kind == ContentItemKind::Folder) {
        return "Folder";
    }
    if (item.kind == ContentItemKind::Problem) {
        return "Problem";
    }
    return std::string(contentTypeOf(item.type).displayName);
}

// How many bytes of `text` fit in `width`, cut at a UTF-8 character boundary.
std::size_t fittingBytes(std::string_view text, float width) {
    std::size_t fits = 0;
    for (std::size_t end = 1; end <= text.size(); ++end) {
        if (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
            continue; // inside a character
        }
        // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): the end pointer bounds the text
        if (ImGui::CalcTextSize(text.data(), text.data() + end).x > width) {
            break;
        }
        fits = end;
    }
    return fits;
}

// The label in at most two lines of `width`, the second ending in "…" when it is cut. The first line
// holds at least one whole UTF-8 character, also when it is wider than the tile.
std::pair<std::string, std::string> tileLines(std::string_view label, float width) {
    std::size_t firstCharacter = label.empty() ? 0 : 1;
    while (firstCharacter < label.size() && (static_cast<unsigned char>(label[firstCharacter]) & 0xC0U) == 0x80U) {
        ++firstCharacter;
    }
    const std::size_t first = std::max(fittingBytes(label, width), firstCharacter);
    if (first >= label.size()) {
        return {std::string(label), {}};
    }
    const std::string_view rest = label.substr(first);
    if (ImGui::CalcTextSize(rest.data(), rest.data() + rest.size()).x <= width) {
        return {std::string(label.substr(0, first)), std::string(rest)};
    }
    constexpr std::string_view kEllipsis = "\xE2\x80\xA6";
    const float ellipsis = ImGui::CalcTextSize(kEllipsis.data(), kEllipsis.data() + kEllipsis.size()).x;
    return {std::string(label.substr(0, first)),
            std::string(rest.substr(0, fittingBytes(rest, width - ellipsis))) + std::string(kEllipsis)};
}

void drawTooltip(const ContentItem& item, bool searching) {
    if (!ImGui::BeginItemTooltip()) {
        return;
    }
    ImGui::TextUnformatted(item.label.c_str());
    ImGui::TextDisabled("%s", typeName(item).c_str());
    ImGui::TextDisabled("%s", item.path.c_str());
    if (searching) {
        ImGui::TextDisabled("in %s", item.folder.empty() ? "Content" : item.folder.c_str());
    }
    for (const ContentBadge& badge : item.problems) {
        const std::string text = iconText(icons::kWarning) + " " + badge.message;
        ImGui::TextUnformatted(text.c_str());
    }
    ImGui::EndTooltip();
}

} // namespace

ContentBrowserPanel::ContentBrowserPanel(EditorSession& session) : m_session(&session), m_model(&session.content()) {}

ContentBrowserPanel::~ContentBrowserPanel() {
    // Enter must not reach a panel that is gone.
    m_session->commands().setPrompt("ith.content.open", {});
}

std::optional<UiRect> ContentBrowserPanel::itemRect(const ContentKey& item) const {
    const auto found = m_itemRects.find(item);
    return found != m_itemRects.end() ? std::optional<UiRect>(found->second) : std::nullopt;
}

std::optional<UiRect> ContentBrowserPanel::folderRect(std::string_view folder) const {
    const auto found = m_folderRects.find(std::string(folder));
    return found != m_folderRects.end() ? std::optional<UiRect>(found->second) : std::nullopt;
}

void ContentBrowserPanel::runCommand(std::string_view id, const Value& args) {
    if (Expected<Value> result = m_session->execute(id, args); !result) {
        ITH_LOG_WARNING(ContentBrowser, "{}", result.error().message);
    }
}

bool ContentBrowserPanel::ask(const ContentKey& item) {
    const ContentItem* target = m_model->find(item);
    if (target == nullptr || target->kind != ContentItemKind::Asset || target->type != kLevelFormat ||
        !m_session->document().dirty() || m_session->document().playing()) {
        return false;
    }
    m_pendingOpen = item;
    m_openQuestion = true;
    return true;
}

bool ContentBrowserPanel::open(const ContentKey& item) {
    if (ask(item)) {
        return true;
    }
    Value args = Value::object();
    args.set("item", keyValue(item));
    runCommand("ith.content.open", args);
    return false;
}

bool ContentBrowserPanel::promptOpen() {
    const ContentKey* primary = m_model->primary();
    return primary != nullptr && ask(*primary);
}

void ContentBrowserPanel::draw() {
    drawBrowser();
    m_panelFocused = m_panelFocused || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const ImVec2 position = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    m_panelRect = ContentScreenRect{.x = position.x, .y = position.y, .width = size.x, .height = size.y};
}

void ContentBrowserPanel::drawBrowser() {
    m_itemRects.clear();
    m_folderRects.clear();
    m_searchRect.reset();
    m_filterRect.reset();
    m_itemsDrawn = 0;
    if (m_model->assets() == nullptr) {
        ImGui::TextDisabled("No project is open");
        return;
    }
    drawToolbar();
    const float scale = displayScale();
    const float statusHeight = ImGui::GetFrameHeightWithSpacing();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float height = std::max(avail.y - statusHeight, ImGui::GetFrameHeight());
    const float splitter = kSplitterWidth * scale;
    const float width = std::max(avail.x - splitter, 1.0f);
    const float share = m_dragTree.value_or(m_model->treeWidth());
    const float treeWidth = std::max(width * share, 1.0f);

    drawTree(treeWidth, height);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::InvisibleButton("##splitter", ImVec2(splitter, height));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (ImGui::IsItemActive()) {
        m_dragTree = std::clamp((treeWidth + ImGui::GetIO().MouseDelta.x) / width, 0.05f, 0.95f);
    } else if (m_dragTree.has_value()) {
        m_model->setTreeWidth(*m_dragTree);
        m_dragTree.reset();
    }
    ImGui::SameLine(0.0f, 0.0f);
    if (ImGui::BeginChild("##view", ImVec2(0.0f, height))) {
        drawPathBar();
        drawItems();
    }
    ImGui::EndChild();
    drawStatus();
    // What the rows asked for, now that nothing iterates over the model.
    const std::vector<std::function<void()>> actions = std::move(m_actions);
    m_actions.clear();
    for (const std::function<void()>& action : actions) {
        action();
    }
}

void ContentBrowserPanel::drawToolbar() {
    // The model keeps at most kMaxContentSearchSize bytes, so the copy is never cut.
    std::array<char, kMaxContentSearchSize + 1> search{};
    m_model->search().copy(search.data(), search.size() - 1);
    if (m_focusSearch) {
        ImGui::SetKeyboardFocusHere();
        m_focusSearch = false;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    const std::string hint = iconText(icons::kSearch) + "  Search";
    if (ImGui::InputTextWithHint("##content_search", hint.c_str(), search.data(), search.size())) {
        runCommand("ith.content.set_filter", stringArgs("search", search.data()));
    }
    m_searchRect = rectOfItem();

    ImGui::SameLine();
    const ContentFilter filter = m_model->filter();
    const std::string filterLabel = iconText(icons::kFilter) + (filter.empty() ? " Filter" : " Filter *");
    const bool filterPressed = ImGui::Button(filterLabel.c_str());
    m_filterRect = rectOfItem();
    if (filterPressed) {
        ImGui::OpenPopup("##content_filter");
    }
    if (ImGui::BeginPopup("##content_filter")) {
        ContentFilter changed = filter;
        for (std::size_t i = 0; i < kContentCategoryCount; ++i) {
            const auto category = static_cast<ContentCategory>(i);
            bool on = filter.has(category);
            const std::string name(displayName(category));
            if (ImGui::Checkbox(name.c_str(), &on)) {
                changed.categories = static_cast<std::uint8_t>(changed.categories ^ (1U << i));
            }
        }
        bool problems = filter.problems;
        if (ImGui::Checkbox("Problems", &problems)) {
            changed.problems = problems;
        }
        if (changed != filter) {
            Value list = Value::array();
            for (std::size_t i = 0; i < kContentCategoryCount; ++i) {
                if (changed.has(static_cast<ContentCategory>(i))) {
                    list.elements().push_back(Value::string(std::string(categoryId(static_cast<ContentCategory>(i)))));
                }
            }
            if (changed.problems) {
                list.elements().push_back(Value::string("problems"));
            }
            Value args = Value::object();
            args.set("categories", std::move(list));
            runCommand("ith.content.set_filter", args);
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    const ContentViewSettings view = m_model->view();
    const bool list = view.mode == ContentViewMode::List;
    const std::string modeLabel = iconText(list ? icons::kGrid : icons::kList);
    if (ImGui::Button(modeLabel.c_str())) {
        runCommand("ith.content.set_view", stringArgs("mode", list ? "grid" : "list"));
    }
    ImGui::SetItemTooltip(list ? "Show the items as a grid" : "Show the items as a list");

    ImGui::SameLine();
    const std::string sortLabel = iconText(icons::kSort) + " Sort";
    if (ImGui::Button(sortLabel.c_str())) {
        ImGui::OpenPopup("##content_sort");
    }
    if (ImGui::BeginPopup("##content_sort")) {
        if (ImGui::RadioButton("Name", view.sortBy == ContentSortKey::Name)) {
            runCommand("ith.content.set_view", stringArgs("sortBy", "name"));
        }
        if (ImGui::RadioButton("Type", view.sortBy == ContentSortKey::Type)) {
            runCommand("ith.content.set_view", stringArgs("sortBy", "type"));
        }
        bool descending = view.descending;
        if (ImGui::Checkbox("Descending", &descending)) {
            Value args = Value::object();
            args.set("descending", Value::boolean(descending));
            runCommand("ith.content.set_view", args);
        }
        ImGui::EndPopup();
    }

    // Rescan last (spec 3.1); it rebuilds every item, so it runs after the draw like the rows' actions.
    ImGui::SameLine();
    const std::string rescan = iconText(icons::kRefresh) + " Rescan";
    if (ImGui::Button(rescan.c_str())) {
        m_actions.emplace_back([this] { runCommand("ith.content.rescan", Value::object()); });
    }
    ImGui::SetItemTooltip("Read the Content folder from disk again");
}

void ContentBrowserPanel::drawTree(float width, float height) {
    if (ImGui::BeginChild("##tree", ImVec2(width, height), ImGuiChildFlags_Borders)) {
        // Changes wait for the end of the draw: they rebuild the rows.
        const std::vector<ContentFolderRow>& rows = m_model->treeRows();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) {
            for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                const ContentFolderRow& row = rows[static_cast<std::size_t>(index)];
                ImGui::PushID(row.path.c_str());
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                     static_cast<float>(row.depth) * ImGui::GetStyle().IndentSpacing);
                // NOLINTBEGIN(bugprone-signed-bitwise): ImGui's flags are int enums
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow |
                                           ImGuiTreeNodeFlags_NoTreePushOnOpen;
                if (!row.hasChildren) {
                    flags |= ImGuiTreeNodeFlags_Leaf;
                }
                if (row.path == m_model->currentFolder()) {
                    flags |= ImGuiTreeNodeFlags_Selected;
                }
                // NOLINTEND(bugprone-signed-bitwise)
                if (row.hasChildren) {
                    ImGui::SetNextItemOpen(row.expanded);
                }
                const std::string label = iconText(icons::kFolder) + " " + row.name;
                const bool open = ImGui::TreeNodeEx("##folder", flags, "%s", label.c_str());
                m_folderRects[row.path] = rectOfItem();
                if (ImGui::IsItemToggledOpen()) {
                    m_actions.emplace_back(
                        [this, path = row.path, expand = open] { m_model->setExpanded(path, expand); });
                } else if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    m_actions.emplace_back(
                        [this, path = row.path, expand = !row.expanded] { m_model->setExpanded(path, expand); });
                } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && row.path != m_model->currentFolder()) {
                    m_actions.emplace_back(
                        [this, path = row.path] { runCommand("ith.content.navigate", stringArgs("folder", path)); });
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
}

void ContentBrowserPanel::drawPathBar() {
    const std::string current = m_model->currentFolder();
    std::size_t start = 0;
    std::string path;
    // A segment draws its name itself: ImGui would hide a "##" in a folder's name.
    const auto segment = [this](const std::string& name, const std::string& target, int id) {
        ImGui::PushID(id);
        const ImVec2 padding = ImGui::GetStyle().FramePadding;
        const ImVec2 text = ImGui::CalcTextSize(name.c_str());
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton("##segment", ImVec2(text.x + padding.x * 2.0f, text.y));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (ImGui::IsItemHovered()) {
            draw->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                ImGui::GetColorU32(ImGuiCol_ButtonHovered), ImGui::GetStyle().FrameRounding);
        }
        draw->AddText(ImVec2(min.x + padding.x, min.y), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
        if (pressed && target != m_model->currentFolder()) {
            m_actions.emplace_back(
                [this, target] { runCommand("ith.content.navigate", stringArgs("folder", target)); });
        }
        ImGui::PopID();
    };
    segment("Content", {}, 0);
    int id = 1;
    while (start < current.size()) {
        std::size_t end = current.find('/', start);
        if (end == std::string::npos) {
            end = current.size();
        }
        path = current.substr(0, end);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", iconText(icons::kChevronRight).c_str());
        ImGui::SameLine();
        segment(current.substr(start, end - start), path, id++);
        start = end + 1;
    }
}

void ContentBrowserPanel::itemInput(const ContentItem& item) {
    const ContentKey key = keyOf(item);
    m_itemRects[key] = rectOfItem();
    ++m_itemsDrawn;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        m_actions.emplace_back([this, key] { open(key); });
    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        m_actions.emplace_back(
            [this, key, mode = std::string(clickMode())] { runCommand("ith.content.select", selectArgs(key, mode)); });
    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !m_model->isSelected(key)) {
        m_actions.emplace_back([this, key] { runCommand("ith.content.select", selectArgs(key, "replace")); });
    }
    drawTooltip(item, m_model->searching());
}

void ContentBrowserPanel::drawItems() {
    if (m_focusView) {
        ImGui::SetNextWindowFocus();
        m_focusView = false;
    }
    if (!ImGui::BeginChild("##items", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoNavInputs)) {
        ImGui::EndChild();
        return;
    }
    if (m_model->shown().empty()) {
        m_columns = 1;
        ImGui::TextDisabled("%s", m_model->searching() ? "No items match" : "This folder is empty.");
    } else if (m_model->view().mode == ContentViewMode::Grid) {
        drawGrid();
    } else {
        m_columns = 1;
        drawList();
    }
    keyboardInput();
    // A click on the empty space between and below the items clears the selection.
    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !m_model->selection().empty()) {
        Value args = Value::object();
        args.set("items", Value::array());
        runCommand("ith.content.select", args);
    }
    ImGui::EndChild();
}

void ContentBrowserPanel::drawGrid() {
    const float scale = displayScale();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 tile(kTileWidth * scale,
                      kTileIconHeight * scale + ImGui::GetTextLineHeight() * 2.0f + style.FramePadding.y * 2.0f);
    const float avail = ImGui::GetContentRegionAvail().x;
    // At least one column, clamped before the conversion (a negative or huge width can't wrap).
    const float columns = std::clamp((avail + style.ItemSpacing.x) / (tile.x + style.ItemSpacing.x), 1.0f, 1024.0f);
    m_columns = static_cast<std::size_t>(columns);
    // Changes wait for the end of the draw: they may rebuild what the model shows.
    const std::vector<const ContentItem*>& shown = m_model->shown();
    const std::size_t rows = (shown.size() + m_columns - 1) / m_columns;
    const float rowHeight = tile.y + style.ItemSpacing.y;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows), rowHeight);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const std::size_t first = static_cast<std::size_t>(row) * m_columns;
            for (std::size_t index = first; index < std::min(first + m_columns, shown.size()); ++index) {
                if (index > first) {
                    ImGui::SameLine();
                }
                ImGui::PushID(static_cast<int>(index));
                drawTile(*shown[index], tile.x, tile.y);
                ImGui::PopID();
            }
        }
    }
    if (m_scrollToFocus) {
        m_scrollToFocus = false;
        const std::optional<ContentKey>& focus = m_model->focusItem();
        const std::optional<std::size_t> index = focus.has_value() ? m_model->shownIndexOf(*focus) : std::nullopt;
        if (index.has_value()) {
            const std::size_t row = *index / m_columns;
            const float top = static_cast<float>(row) * rowHeight;
            const float visible = ImGui::GetWindowHeight();
            if (top < ImGui::GetScrollY()) {
                ImGui::SetScrollY(top);
            } else if (top + rowHeight > ImGui::GetScrollY() + visible) {
                ImGui::SetScrollY(top + rowHeight - visible);
            }
        }
    }
}

void ContentBrowserPanel::drawTile(const ContentItem& item, float width, float height) {
    const ImVec2 size(width, height);
    const float scale = displayScale();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float lineHeight = ImGui::GetTextLineHeight();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##tile", size, kTileButtons);
    const ImVec2 max(min.x + size.x, min.y + size.y);
    const bool selected = m_model->isSelected(keyOf(item));
    if (selected || ImGui::IsItemHovered()) {
        draw->AddRectFilled(min, max, ImGui::GetColorU32(selected ? ImGuiCol_Header : ImGuiCol_HeaderHovered),
                            style.FrameRounding);
    }
    const ImU32 color =
        ImGui::GetColorU32(item.kind == ContentItemKind::Problem ? ImGuiCol_TextDisabled : ImGuiCol_Text);
    // The icon, twice the text size, centred above the label.
    ImGui::PushFont(nullptr, style.FontSizeBase * 2.0f);
    const std::string icon = iconText(iconOf(item));
    const ImVec2 iconSize = ImGui::CalcTextSize(icon.c_str());
    draw->AddText(ImVec2(min.x + (size.x - iconSize.x) * 0.5f,
                         min.y + (kTileIconHeight * scale - iconSize.y) * 0.5f + style.FramePadding.y),
                  color, icon.c_str());
    ImGui::PopFont();
    if (item.hasProblems()) {
        const std::string badge = iconText(icons::kWarning);
        draw->AddText(ImVec2(max.x - lineHeight - style.FramePadding.x, min.y + style.FramePadding.y),
                      ImGui::GetColorU32(ImGuiCol_PlotHistogram), badge.c_str());
    }
    const auto [first, second] = tileLines(item.label, size.x - style.FramePadding.x * 2.0f);
    float y = min.y + kTileIconHeight * scale + style.FramePadding.y;
    for (const std::string* line : {&first, &second}) {
        if (!line->empty()) {
            const float lineWidth = ImGui::CalcTextSize(line->c_str()).x;
            draw->AddText(ImVec2(min.x + (size.x - lineWidth) * 0.5f, y), color, line->c_str());
            y += lineHeight;
        }
    }
    itemInput(item);
}

void ContentBrowserPanel::drawList() {
    const bool searching = m_model->searching();
    const ContentViewSettings view = m_model->view();
    // ImGui keeps a table's sort itself; a table per sort makes its headers show the model's.
    ImGui::PushID(static_cast<int>(view.sortBy) * 2 + (view.descending ? 1 : 0));
    if (!ImGui::BeginTable("##content_list", searching ? 3 : 2, kListTable)) {
        ImGui::PopID();
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    // NOLINTBEGIN(bugprone-signed-bitwise): ImGui's flags are int enums
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | sortFlags(view, ContentSortKey::Name));
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed | sortFlags(view, ContentSortKey::Type),
                            ImGui::GetFontSize() * 9.0f);
    if (searching) {
        ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort);
    }
    // NOLINTEND(bugprone-signed-bitwise)
    ImGui::TableHeadersRow();
    listSorting();
    const std::vector<const ContentItem*>& shown = m_model->shown();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(shown.size()));
    if (m_scrollToFocus) {
        m_scrollToFocus = false;
        const std::optional<ContentKey>& focus = m_model->focusItem();
        const std::optional<std::size_t> index = focus.has_value() ? m_model->shownIndexOf(*focus) : std::nullopt;
        if (index.has_value()) {
            clipper.IncludeItemByIndex(static_cast<int>(*index));
            m_scrollToFocus = true; // scrolled once the row is drawn
        }
    }
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            ImGui::PushID(row);
            drawListRow(*shown[static_cast<std::size_t>(row)], searching);
            ImGui::PopID();
        }
    }
    m_scrollToFocus = false;
    ImGui::EndTable();
    ImGui::PopID();
}

void ContentBrowserPanel::listSorting() {
    ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
    if (specs == nullptr || !specs->SpecsDirty) {
        return;
    }
    specs->SpecsDirty = false;
    if (specs->SpecsCount < 1) {
        return;
    }
    const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
    ContentViewSettings view = m_model->view();
    view.sortBy = spec.ColumnIndex == 1 ? ContentSortKey::Type : ContentSortKey::Name;
    view.descending = spec.SortDirection == ImGuiSortDirection_Descending;
    if (view == m_model->view()) {
        return; // a new table reporting the sort it was made with
    }
    Value args = Value::object();
    args.set("sortBy", Value::string(view.sortBy == ContentSortKey::Type ? "type" : "name"));
    args.set("descending", Value::boolean(view.descending));
    m_actions.emplace_back([this, args = std::move(args)] { runCommand("ith.content.set_view", args); });
}

void ContentBrowserPanel::drawListRow(const ContentItem& item, bool searching) {
    const ContentKey key = keyOf(item);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    // The row is an unlabelled selectable with the label drawn after it: ImGui would hide a "##" in a
    // name.
    const float start = ImGui::GetCursorPosX();
    ImGui::Selectable("##row", m_model->isSelected(key), kListRow);
    if (m_scrollToFocus && m_model->focusItem() == key) {
        ImGui::SetScrollHereY();
        m_scrollToFocus = false;
    }
    itemInput(item);
    ImGui::SameLine(start);
    const std::string label = iconText(iconOf(item)) + " " + item.label +
                              (item.hasProblems() ? "  " + iconText(icons::kWarning) : std::string());
    if (item.kind == ContentItemKind::Problem) {
        ImGui::TextDisabled("%s", label.c_str());
    } else {
        ImGui::TextUnformatted(label.c_str());
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(typeName(item).c_str());
    if (searching) {
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(item.folder.empty() ? "Content" : item.folder.c_str());
    }
}

// The asset view's widget keys. The keymap consumer consumes the same chords while the browser has the
// focus (kContentWidgetChords in editor_core's Keymap.h), so they never also run a shortcut; keep the
// two in step.
void ContentBrowserPanel::keyboardInput() {
    const ImGuiIO& io = ImGui::GetIO();
    // The list's table scrolls in a child window of its own.
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) || io.WantTextInput) {
        return;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        m_focusSearch = true;
        return;
    }
    const std::vector<const ContentItem*>& shown = m_model->shown();
    if (shown.empty() || io.KeyAlt || io.KeyCtrl) {
        return; // Alt + Up is the parent folder, a keymap binding
    }
    const bool grid = m_model->view().mode == ContentViewMode::Grid;
    const auto count = static_cast<std::int64_t>(shown.size());
    const auto columns = static_cast<std::int64_t>(m_columns);
    std::optional<std::int64_t> step;
    std::optional<std::int64_t> to;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && grid) {
        step = -1;
    } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && grid) {
        step = 1;
    } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        step = -columns;
    } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        step = columns;
    } else if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
        to = 0;
    } else if (ImGui::IsKeyPressed(ImGuiKey_End, false)) {
        to = count - 1;
    }
    if (!step.has_value() && !to.has_value()) {
        return;
    }
    const std::optional<ContentKey>& focus = m_model->focusItem();
    const std::optional<std::size_t> current = focus.has_value() ? m_model->shownIndexOf(*focus) : std::nullopt;
    std::int64_t target = 0;
    if (to.has_value()) {
        target = *to;
    } else if (current.has_value()) {
        target = std::clamp(static_cast<std::int64_t>(*current) + *step, std::int64_t{0}, count - 1);
    }
    const ContentKey key = keyOf(*shown[static_cast<std::size_t>(target)]);
    runCommand("ith.content.select", selectArgs(key, io.KeyShift ? "range" : "replace"));
    m_scrollToFocus = true;
}

void ContentBrowserPanel::drawStatus() {
    ImGui::Text("%zu items", m_model->shown().size());
    if (const std::size_t selected = m_model->selection().size(); selected > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("%zu selected", selected);
    }
    if (const std::size_t problems = m_model->shownProblems(); problems > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        const std::string label = iconText(icons::kWarning) + " " + std::to_string(problems) + " problems";
        if (ImGui::SmallButton(label.c_str())) {
            Value list = Value::array();
            list.elements().push_back(Value::string("problems"));
            Value args = Value::object();
            args.set("categories", std::move(list));
            runCommand("ith.content.set_filter", args);
        }
        ImGui::SetItemTooltip("Show the items with problems");
    }
}

void ContentBrowserPanel::drawUnsavedQuestion() {
    if (m_openQuestion) {
        ImGui::OpenPopup(kUnsavedPopup);
        m_openQuestion = false;
    }
    if (!ImGui::BeginPopupModal(kUnsavedPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!ImGui::IsPopupOpen(kUnsavedPopup)) {
            m_pendingOpen.reset();
        }
        return;
    }
    ImGui::TextUnformatted("The level has unsaved changes. Save them before opening another level?");
    std::optional<std::string_view> answer;
    bool done = false;
    if (ImGui::Button("Save")) {
        answer = "save";
    }
    ImGui::SameLine();
    if (ImGui::Button("Don't Save")) {
        answer = "discard";
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        done = true;
    }
    if (answer.has_value() && m_pendingOpen.has_value()) {
        Value args = Value::object();
        args.set("item", keyValue(*m_pendingOpen));
        args.set("unsaved", Value::string(std::string(*answer)));
        runCommand("ith.content.open", args);
        done = true;
    }
    if (done) {
        m_pendingOpen.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ContentBrowserPanel::drawDrawer() {
    if (m_pendingOpen.has_value() || ImGui::IsPopupOpen(kUnsavedPopup)) {
        drawUnsavedQuestion();
    }
    // One browser on screen at a time (ADR-0049 rule 7): the panel, once in the layout, wins.
    if (m_model->drawerOpen() && isPanelOpen(m_session->layouts().current(), kContentBrowserPanelId)) {
        m_model->setDrawerOpen(false);
    }
    bool drawerFocused = false;
    m_drawerRect.reset();
    if (m_model->drawerOpen()) {
        if (!m_drawerWasOpen) {
            m_focusView = true;
        }
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float scale = displayScale();
        const float statusHeight = ImGui::GetFrameHeight();
        const float maxHeight = viewport->WorkSize.y * kDrawerMaxShare;
        const float minHeight = std::min(kDrawerMinHeight * scale, maxHeight);
        const float saved = m_model->drawerHeight() > 0.0f ? m_model->drawerHeight() * scale
                                                           : viewport->WorkSize.y * kDrawerDefaultShare;
        const float height = std::clamp(m_dragHeight.value_or(saved), minHeight, maxHeight);
        ImGui::SetNextWindowPos(
            ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - statusHeight - height));
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));
        ImGui::SetNextWindowViewport(viewport->ID);
        if (!m_drawerWasOpen) {
            ImGui::SetNextWindowFocus();
        }
        // NOLINTBEGIN(bugprone-signed-bitwise): ImGui's flags are int enums
        constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
        // NOLINTEND(bugprone-signed-bitwise)
        bool hovered = false;
        if (ImGui::Begin("Content Drawer###ith.content_drawer", nullptr, kFlags)) {
            // The top edge resizes the drawer.
            const ImVec2 cursor = ImGui::GetCursorPos();
            ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
            ImGui::InvisibleButton("##drawer_edge",
                                   ImVec2(std::max(ImGui::GetWindowWidth(), 1.0f), kSplitterWidth * scale));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            }
            if (ImGui::IsItemActive()) {
                m_dragHeight = std::clamp(height - ImGui::GetIO().MouseDelta.y, minHeight, maxHeight);
            } else if (m_dragHeight.has_value()) {
                m_model->setDrawerHeight(*m_dragHeight / scale);
                m_dragHeight.reset();
            }
            ImGui::SetCursorPos(cursor);
            drawBrowser();
            // NOLINTBEGIN(bugprone-signed-bitwise): ImGui's flags are int enums
            drawerFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                             ImGuiHoveredFlags_AllowWhenBlockedByPopup |
                                             ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            // NOLINTEND(bugprone-signed-bitwise)
        }
        m_drawerRect = UiRect{.x = viewport->WorkPos.x,
                              .y = viewport->WorkPos.y + viewport->WorkSize.y - statusHeight - height,
                              .width = viewport->WorkSize.x,
                              .height = height};
        ImGui::End();
        // A click outside closes it, unless a popup (its menus, a dialog) is open.
        if (m_drawerWasOpen && !hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !ImGui::IsPopupOpen("", kAnyPopup)) {
            Value args = Value::object();
            args.set("open", Value::boolean(false));
            runCommand("ith.content.toggle_drawer", args);
        }
    }
    m_drawerWasOpen = m_model->drawerOpen();
    m_model->setKeyboardFocus(m_panelFocused || drawerFocused);
    // Where the browser was drawn this frame, for the presses before the next frame.
    std::vector<ContentScreenRect> rects;
    if (m_panelRect.has_value()) {
        rects.push_back(*m_panelRect);
    }
    if (m_drawerRect.has_value()) {
        rects.push_back(ContentScreenRect{
            .x = m_drawerRect->x, .y = m_drawerRect->y, .width = m_drawerRect->width, .height = m_drawerRect->height});
    }
    m_model->setScreenRects(std::move(rects));
    m_panelRect.reset();
    m_panelFocused = false;
}

void registerContentBrowser(EditorSession& session, ContentBrowserPanel& panel) {
    session.panels().add({.id = std::string(kContentBrowserPanelId),
                          .displayName = "Content Browser",
                          .defaultDock = DockPlace::Bottom,
                          .draw = [&panel](std::string_view /*instance*/) { panel.draw(); }});
    session.commands().setPrompt("ith.content.open", [&panel](EditorSession&) { return panel.promptOpen(); });
}

} // namespace ith
