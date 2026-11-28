#include "ith/editor_ui/ContentBrowserPanel.h"

#include "ith/asset_database/AssetDatabase.h"
#include "ith/asset_database/AssetHeader.h"
#include "ith/asset_database/Importer.h"
#include "ith/asset_database/Sidecar.h"
#include "ith/editor_core/CommandRegistry.h"
#include "ith/editor_core/EditorLayout.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/PanelRegistry.h"
#include "ith/editor_ui/Icons.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/Log.h"
#include "ith/foundation/StringUtils.h"
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
// The items the delete confirmation lists before "and <k> more" (spec 3.11).
constexpr std::size_t kDeleteListed = 20;

constexpr const char* kUnsavedPopup = "Unsaved Changes###ith.content_unsaved";
constexpr const char* kDeletePopupId = "###ith.content_delete";
constexpr const char* kConflictPopup = "Import Conflicts###ith.content_conflicts";
constexpr const char* kMenuPopup = "##content_menu";
constexpr const char* kAddPopup = "##content_add";
// The drag and drop payload of the browser's items; the items themselves are the panel's m_dragged.
constexpr const char* kItemsPayload = "ith.content.items";

constexpr std::string_view kContentBrowserPanelId = "ith.content_browser";

// The status line when no file dialog can be shown (ADR-0049 rule 12).
constexpr std::string_view kNoDialog = "The file dialog can't be shown: drop files onto the browser to import them";
// The status line when files arrive while a question of the browser waits for its answer.
constexpr std::string_view kAnswerFirst = "Answer or cancel the open question first, then import the files again";
// The status line when files arrive while an import waits for its frame.
constexpr std::string_view kImportWaiting = "An import is waiting to run: import these files again after it";

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
// The rename field: Enter reports itself, and the name is selected when it opens.
constexpr ImGuiInputTextFlags kRenameField = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
// The dialogs fit their content.
constexpr ImGuiWindowFlags kDialog = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
// The delete dialog's lists: as tall as their content, up to the size constraints.
constexpr ImGuiChildFlags kListChild = ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders;
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

// The part of the last item inside its window: rows scrolled under the path bar or out of a child
// window must not take a drop there.
UiRect visibleRectOfItem() {
    const ImVec2 position = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    const float left = std::max(itemMin.x, position.x);
    const float top = std::max(itemMin.y, position.y);
    const float right = std::min(itemMax.x, position.x + size.x);
    const float bottom = std::min(itemMax.y, position.y + size.y);
    return UiRect{.x = left, .y = top, .width = std::max(right - left, 0.0f), .height = std::max(bottom - top, 0.0f)};
}

bool contains(const UiRect& rect, float x, float y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
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

Value keyList(const std::vector<ContentKey>& keys) {
    Value list = Value::array();
    for (const ContentKey& key : keys) {
        list.elements().push_back(keyValue(key));
    }
    return list;
}

Value selectArgs(const ContentKey& key, std::string_view mode) {
    Value args = Value::object();
    args.set("items", keyList({key}));
    args.set("mode", Value::string(std::string(mode)));
    return args;
}

Value stringArgs(std::string_view key, std::string_view value) {
    Value args = Value::object();
    args.set(std::string(key), Value::string(std::string(value)));
    return args;
}

Value itemArgs(const ContentKey& key) {
    Value args = Value::object();
    args.set("item", keyValue(key));
    return args;
}

Value stringList(const std::vector<std::string>& texts) {
    Value list = Value::array();
    for (const std::string& text : texts) {
        list.elements().push_back(Value::string(text));
    }
    return list;
}

// A member of a result: its text, empty when it isn't a string.
std::string_view textOf(const Value& value, std::string_view key) {
    const Value* member = value.kind() == Value::Kind::Object ? value.find(key) : nullptr;
    return member != nullptr && member->kind() == Value::Kind::String ? std::string_view(member->asString())
                                                                      : std::string_view();
}

bool flagOf(const Value& value, std::string_view key) {
    const Value* member = value.kind() == Value::Kind::Object ? value.find(key) : nullptr;
    return member != nullptr && member->kind() == Value::Kind::Bool && member->asBool();
}

std::uint64_t numberOf(const Value& value, std::string_view key) {
    const Value* member = value.kind() == Value::Kind::Object ? value.find(key) : nullptr;
    return member != nullptr ? member->toNumber<std::uint64_t>().value_or(0) : 0;
}

// The elements of an array member; empty when it isn't one.
const std::vector<Value>& listOf(const Value& value, std::string_view key) {
    static const std::vector<Value> kNone;
    const Value* member = value.kind() == Value::Kind::Object ? value.find(key) : nullptr;
    return member != nullptr && member->kind() == Value::Kind::Array ? member->elements() : kNone;
}

// "1 file", "3 files".
std::string counted(std::size_t count, std::string_view noun) {
    return std::to_string(count) + " " + std::string(noun) + (count == 1 ? "" : "s");
}

// What the status line says after a file operation: what it did and what failed; empty when there
// is nothing to say (a prompt started, a move or duplicate that went through).
std::string resultNotice(std::string_view id, const Value& result) {
    const std::vector<Value>& failed = listOf(result, "failed");
    const std::vector<Value>& skipped = listOf(result, "skipped");
    const auto tail = [&] {
        std::string text;
        if (!skipped.empty()) {
            text += ", " + std::to_string(skipped.size()) + " skipped";
        }
        if (!failed.empty()) {
            text += ", " + std::to_string(failed.size()) + " failed";
        }
        return text + (skipped.empty() && failed.empty() ? "" : " (see the Output Log)");
    };
    if (id == "ith.content.import" && result.find("imported") != nullptr) {
        return "Imported " + counted(listOf(result, "imported").size(), "file") + tail();
    }
    if (id == "ith.content.reimport" && result.find("reimported") != nullptr) {
        return "Reimported " + counted(listOf(result, "reimported").size(), "file") + tail();
    }
    if (id == "ith.content.delete" && result.find("deleted") != nullptr &&
        result.find("deleted")->kind() == Value::Kind::Array) {
        return "Deleted " + counted(listOf(result, "deleted").size(), "item") + tail();
    }
    if (!failed.empty()) {
        const Value& first = failed.front();
        const std::string_view where = textOf(first, "path");
        return std::string(where) + (where.empty() ? "" : ": ") + std::string(textOf(first, "message"));
    }
    return {};
}

// The file operations, whose results go to the status line.
bool isFileOperation(std::string_view id) {
    constexpr std::array<std::string_view, 8> kOperations = {
        "ith.content.new_folder", "ith.content.rename", "ith.content.move",     "ith.content.duplicate",
        "ith.content.delete",     "ith.content.import", "ith.content.reimport", "ith.content.fix"};
    return std::ranges::find(kOperations, id) != kOperations.end();
}

// The selection mode of a click with the modifiers held.
std::string_view clickMode() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyShift) {
        return io.KeyCtrl ? "add_range" : "range";
    }
    return io.KeyCtrl ? "toggle" : "replace";
}

// Whether the item just drawn was clicked: on the press for an item not selected, with Ctrl or Shift
// held, or when it is the whole selection; on the release without a drag for an item of a larger
// selection, so dragging it keeps the whole selection (spec 3.9).
bool selectsOnClick(bool selectedAmongOthers) {
    const ImGuiIO& io = ImGui::GetIO();
    if (!selectedAmongOthers || io.KeyCtrl || io.KeyShift) {
        return ImGui::IsItemClicked(ImGuiMouseButton_Left);
    }
    const float threshold = io.MouseDragThreshold * io.MouseDragThreshold;
    return ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
           io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < threshold;
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

// The delete preview's items (spec 3.11): at most kDeleteListed, with the contents of folders, then
// how many more.
void drawDeleteItems(const std::vector<Value>& items) {
    for (std::size_t i = 0; i < std::min(items.size(), kDeleteListed); ++i) {
        const Value& item = items[i];
        const bool folder = textOf(item, "kind") == "folder";
        const std::string label =
            iconText(folder ? icons::kFolder : icons::kFile) + " " + std::string(textOf(item, "label"));
        ImGui::TextUnformatted(label.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", std::string(textOf(item, "path")).c_str());
        if (folder) {
            std::string inside = "    " + counted(numberOf(item, "assets"), "asset") + " inside";
            if (const std::uint64_t others = numberOf(item, "otherFiles"); others > 0) {
                inside += ", and " + counted(others, "other file") + " not shown in the Content Browser";
            }
            ImGui::TextDisabled("%s", inside.c_str());
        }
    }
    if (items.size() > kDeleteListed) {
        ImGui::TextDisabled("and %zu more", items.size() - kDeleteListed);
    }
}

// Each used asset of the delete preview with its users, the open level and its unsaved changes
// named, under the warning wrapped at `width`.
void drawUses(const std::vector<Value>& uses, float width) {
    if (uses.empty()) {
        return;
    }
    ImGui::Separator();
    const std::string warning = iconText(icons::kWarning) +
                                " These assets are used. The references to them will become missing and load as "
                                "placeholders:";
    ImGui::PushTextWrapPos(width);
    ImGui::TextUnformatted(warning.c_str());
    ImGui::PopTextWrapPos();
    for (const Value& use : uses) {
        const Value* asset = use.find("asset");
        const std::string used = std::string(asset != nullptr ? textOf(*asset, "label") : "") + " (" +
                                 std::string(asset != nullptr ? textOf(*asset, "path") : "") + ") is used by:";
        ImGui::TextUnformatted(used.c_str());
        for (const Value& user : listOf(use, "users")) {
            std::string line =
                "    " + std::string(textOf(user, "label")) + " (" + std::string(textOf(user, "path")) + ")";
            if (flagOf(user, "unsaved")) {
                line += ", the open level's unsaved changes";
            } else if (flagOf(user, "openLevel")) {
                line += ", the open level";
            }
            ImGui::TextUnformatted(line.c_str());
        }
    }
}

// The files the delete preview couldn't read while looking for users, with their errors.
void drawUnreadable(const std::vector<Value>& unreadable) {
    if (unreadable.empty()) {
        return;
    }
    ImGui::Separator();
    ImGui::TextUnformatted("These files couldn't be read while looking for users:");
    for (const Value& file : unreadable) {
        const std::string line =
            "    " + std::string(textOf(file, "path")) + ": " + std::string(textOf(file, "message"));
        ImGui::TextUnformatted(line.c_str());
    }
}

// The patterns of the file dialog's first filter (ADR-0049 rule 12): every extension the importers
// read and the native asset extensions, without dots, each also in upper case: SDL hands the desktop
// portal "*.<pattern>" as it is, and whether the portal's backend ignores case varies (the Windows
// dialog and zenity ignore it, so the copies cost nothing there).
std::string importPatterns(const AssetDatabase* assets) {
    std::vector<std::string_view> extensions;
    if (assets != nullptr) {
        extensions = assets->importers().extensions();
    }
    extensions.push_back(kNativeAssetExtension);
    extensions.push_back(kLevelExtension);
    std::string patterns;
    for (const std::string_view extension : extensions) {
        const std::string_view bare = extension.substr(extension.starts_with('.') ? 1 : 0);
        std::string upper(bare);
        std::ranges::transform(upper, upper.begin(),
                               [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; });
        patterns += (patterns.empty() ? "" : ";") + std::string(bare);
        if (upper != bare) {
            patterns += ";" + upper;
        }
    }
    return patterns;
}

} // namespace

ContentBrowserPanel::ContentBrowserPanel(EditorSession& session, UiHost* host)
    : m_session(&session), m_model(&session.content()), m_host(host), m_choices(Value::object()) {}

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

std::optional<UiRect> ContentBrowserPanel::controlRect(std::string_view label) const {
    const auto found = m_controls.find(std::string(label));
    return found != m_controls.end() ? std::optional<UiRect>(found->second) : std::nullopt;
}

std::optional<UiRect> ContentBrowserPanel::segmentRect(std::string_view folder) const {
    const auto found =
        std::ranges::find_if(m_segments, [folder](const DropTarget& segment) { return segment.folder == folder; });
    return found != m_segments.end() ? std::optional<UiRect>(found->rect) : std::nullopt;
}

void ContentBrowserPanel::recordControl(std::string_view label) {
    m_frameControls[std::string(label)] = rectOfItem();
}

std::optional<Value> ContentBrowserPanel::runCommand(std::string_view id, const Value& args) {
    Expected<Value> result = m_session->execute(id, args);
    if (!result) {
        ITH_LOG_WARNING(ContentBrowser, "{}", result.error().message);
        m_notice = result.error().message;
        return std::nullopt;
    }
    if (isFileOperation(id)) {
        m_notice = resultNotice(id, *result);
    }
    return std::move(*result);
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
    runCommand("ith.content.open", itemArgs(item));
    return false;
}

bool ContentBrowserPanel::promptOpen() {
    const ContentKey* primary = m_model->primary();
    return primary != nullptr && ask(*primary);
}

// --- import: the file dialog, operating-system drops, the queued import -------------------------

void ContentBrowserPanel::dialogUnavailable(std::string_view error) {
    ITH_LOG_WARNING(ContentBrowser, "The file dialog can't be shown: {}", error);
    m_notice = std::string(kNoDialog);
}

bool ContentBrowserPanel::questionOpen() const {
    return m_dialog != ContentDialog::None || m_pendingOpen.has_value() || m_model->prompt() != nullptr;
}

void ContentBrowserPanel::refuseWhileAsking(std::string_view what) {
    ITH_LOG_WARNING(ContentBrowser, "{} not imported: a question of the Content Browser is open", what);
    m_notice = std::string(kAnswerFirst);
}

void ContentBrowserPanel::showImportDialog() {
    if (questionOpen()) {
        m_notice = std::string(kAnswerFirst);
        return;
    }
    if (m_host == nullptr) {
        dialogUnavailable("this editor has no file dialog");
        return;
    }
    if (m_dialogOpen) {
        dialogUnavailable("a file dialog is open already");
        return;
    }
    const std::string patterns = importPatterns(m_model->assets());
    const std::array<FileDialogFilter, 2> filters = {FileDialogFilter{.name = "Importable files", .patterns = patterns},
                                                     FileDialogFilter{.name = "All files", .patterns = "*"}};
    const Expected<void> shown = m_host->showOpenFileDialog(
        OpenFileDialogDesc{.filters = filters, .defaultFolder = m_lastImportFolder, .allowMany = true});
    if (!shown) {
        dialogUnavailable(shown.error().message);
        return;
    }
    m_dialogOpen = true;
    m_dialogTarget = m_model->currentFolder();
    m_dialogFiles.clear();
}

void ContentBrowserPanel::dialogFileChosen(std::string_view path) {
    if (m_dialogOpen && !path.empty()) {
        m_dialogFiles.emplace_back(path);
    }
}

void ContentBrowserPanel::dialogClosed(std::string_view error) {
    if (!m_dialogOpen) {
        return;
    }
    m_dialogOpen = false;
    const std::vector<std::string> files = std::move(m_dialogFiles);
    m_dialogFiles.clear();
    if (!error.empty()) {
        dialogUnavailable(error);
        return;
    }
    if (files.empty()) {
        return; // cancelled
    }
    if (questionOpen()) {
        refuseWhileAsking("The files chosen in the file dialog were");
        return;
    }
    // A name no fs function can address (ADR-0052 item 5) is no folder to start the next dialog in.
    if (!fs::hasUnusableName(files.front())) {
        m_lastImportFolder = std::string(fs::parentPath(fs::normalizePath(files.front())));
    }
    Value args = Value::object();
    args.set("files", stringList(files));
    args.set("folder", Value::string(m_dialogTarget));
    queueImport(std::move(args));
}

void ContentBrowserPanel::dragEntered() {
    m_droppedFiles.clear();
    m_osDrag.reset();
    m_dragPositioned = false;
}

void ContentBrowserPanel::dragMoved(float x, float y) {
    m_osDrag = std::pair(x, y);
    m_dragPositioned = true;
}

void ContentBrowserPanel::fileDropped(std::string_view path, float x, float y) {
    if (path.empty()) {
        return;
    }
    if (m_droppedFiles.empty()) {
        m_dropPoint = std::pair(x, y);
    }
    m_droppedFiles.emplace_back(path);
    m_osDrag = std::pair(x, y);
}

void ContentBrowserPanel::dropEnded() {
    const std::vector<std::string> files = std::move(m_droppedFiles);
    const bool positioned = m_dragPositioned;
    m_droppedFiles.clear();
    m_osDrag.reset();
    m_dropHighlight.reset();
    m_dragPositioned = false;
    if (files.empty()) {
        return;
    }
    if (questionOpen()) {
        refuseWhileAsking("The dropped files were");
        return;
    }
    std::optional<std::string> folder;
    if (positioned) {
        folder = dropFolderAt(m_dropPoint.first, m_dropPoint.second);
        if (!folder.has_value()) {
            return; // dropped outside the browser (ADR-0049 rule 26)
        }
    } else if (!m_browserRects.empty()) {
        // Windows without its drag and drop service (OLE) reports no position: the browser on screen
        // takes the drop into its current folder.
        folder = m_model->currentFolder();
    } else {
        ITH_LOG_WARNING(ContentBrowser, "Dropped files without a position were not imported: the Content Browser "
                                        "is not shown");
        m_notice = "Open the Content Browser, then drop the files onto it";
        return;
    }
    Value args = Value::object();
    args.set("files", stringList(files));
    args.set("folder", Value::string(*folder));
    queueImport(std::move(args));
}

std::optional<ContentBrowserPanel::DropTarget> ContentBrowserPanel::dropTargetAt(float x, float y) const {
    const auto browser =
        std::ranges::find_if(m_browserRects, [x, y](const UiRect& rect) { return contains(rect, x, y); });
    if (browser == m_browserRects.end()) {
        return std::nullopt;
    }
    for (const DropTarget& segment : m_segments) {
        if (contains(segment.rect, x, y)) {
            return segment;
        }
    }
    for (const auto& [path, rect] : m_folderRects) {
        if (contains(rect, x, y)) {
            return DropTarget{.folder = path, .rect = rect};
        }
    }
    for (const auto& [key, rect] : m_itemRects) {
        const ContentItem* item = m_model->find(key);
        if (item != nullptr && item->kind == ContentItemKind::Folder && contains(rect, x, y)) {
            return DropTarget{.folder = item->path, .rect = rect};
        }
    }
    return DropTarget{.folder = m_model->currentFolder(), .rect = *browser};
}

std::optional<std::string> ContentBrowserPanel::dropFolderAt(float x, float y) const {
    std::optional<DropTarget> target = dropTargetAt(x, y);
    return target.has_value() ? std::optional<std::string>(std::move(target->folder)) : std::nullopt;
}

void ContentBrowserPanel::queueImport(Value args) {
    const std::size_t files = listOf(args, "files").size();
    if (m_pendingImport.has_value()) {
        // One import at a time; the second is refused out loud, never dropped silently.
        ITH_LOG_WARNING(ContentBrowser, "{} not imported: another import is waiting to run", counted(files, "file"));
        m_notice = std::string(kImportWaiting);
        return;
    }
    m_pendingImport = PendingImport{.args = std::move(args), .queuedFrame = ImGui::GetFrameCount()};
    m_notice = "Importing " + counted(files, "file") + "\xE2\x80\xA6";
}

void ContentBrowserPanel::runPendingImport() {
    // The frame after the queueing one drew the notice; the import runs before anything else in the
    // frame after that, so no frame is drawn while it runs (ADR-0049 rule 11).
    if (!m_pendingImport.has_value() || ImGui::GetFrameCount() <= m_pendingImport->queuedFrame + 1) {
        return;
    }
    const Value args = std::move(m_pendingImport->args);
    m_pendingImport.reset();
    runCommand("ith.content.import", args);
}

// --- drawing ------------------------------------------------------------------------------------

void ContentBrowserPanel::draw() {
    runPendingImport();
    drawBrowser();
    m_panelFocused = m_panelFocused || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const ImVec2 position = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    m_panelRect = ContentScreenRect{.x = position.x, .y = position.y, .width = size.x, .height = size.y};
}

void ContentBrowserPanel::drawBrowser() {
    m_itemRects.clear();
    m_folderRects.clear();
    m_segments.clear();
    m_searchRect.reset();
    m_filterRect.reset();
    m_itemsDrawn = 0;
    if (m_model->assets() == nullptr) {
        ImGui::TextDisabled("No project is open");
        return;
    }
    // The inline rename follows the model's prompt: a new one fills the field and takes the focus.
    const ContentPrompt* prompt = m_model->prompt();
    if (prompt != nullptr && prompt->kind == ContentPromptKind::Rename) {
        if (m_renameItem != prompt->item) {
            m_renameItem = prompt->item;
            m_renameText.fill('\0');
            const std::string_view name = textOf(prompt->details, "name");
            // Cut at a UTF-8 character boundary when longer than the field (never on a real name).
            std::size_t size = std::min(name.size(), m_renameText.size() - 1);
            while (size > 0 && size < name.size() && (static_cast<unsigned char>(name[size]) & 0xC0U) == 0x80U) {
                --size;
            }
            name.substr(0, size).copy(m_renameText.data(), size);
            m_renameExtension = std::string(textOf(prompt->details, "extension"));
            m_renameFocus = true;
            m_renameError.clear();
            m_scrollToFocus = m_model->focusItem() == prompt->item;
            // A folder the asset view doesn't show is renamed on its tree row: expand the folders
            // above it, so the row and the field are drawn this frame (nothing iterates the rows yet).
            if (prompt->item.id.isNil() && !m_model->shownIndexOf(prompt->item).has_value()) {
                const std::string path = prompt->item.path;
                m_model->revealInTree(path);
            }
        }
    } else {
        m_renameItem.reset();
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
    drawContextMenu();
    // What the rows asked for, now that nothing iterates over the model.
    const std::vector<std::function<void()>> actions = std::move(m_actions);
    m_actions.clear();
    for (const std::function<void()>& action : actions) {
        action();
    }
}

void ContentBrowserPanel::drawToolbar() {
    const std::string import = iconText(icons::kImport) + " Import";
    if (ImGui::Button(import.c_str())) {
        m_actions.emplace_back([this] { showImportDialog(); });
    }
    recordControl("Import");
    ImGui::SetItemTooltip("Copy files into this folder and import them");
    ImGui::SameLine();
    const std::string add = iconText(icons::kPlus) + " Add";
    if (ImGui::Button(add.c_str())) {
        ImGui::OpenPopup(kAddPopup);
    }
    recordControl("Add");
    if (ImGui::BeginPopup(kAddPopup)) {
        const std::string newFolder = iconText(icons::kFolderPlus) + " New Folder";
        if (ImGui::MenuItem(newFolder.c_str())) {
            m_actions.emplace_back([this] { runCommand("ith.content.new_folder", Value::object()); });
        }
        recordControl("New Folder");
        ImGui::EndPopup();
    }

    ImGui::SameLine();
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
    drawFilterMenu(filter);

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

void ContentBrowserPanel::drawFilterMenu(const ContentFilter& filter) {
    if (!ImGui::BeginPopup("##content_filter")) {
        return;
    }
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

void ContentBrowserPanel::drawTree(float width, float height) {
    const bool visible = ImGui::BeginChild("##tree", ImVec2(width, height), ImGuiChildFlags_Borders);
    // F2 and Delete act on the tree's folder while it has the focus (spec 3.6).
    m_treeFocused = m_treeFocused || ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    // Where the tree was drawn, so a press in it between frames gives it the focus.
    const ImVec2 position = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    m_treeRects.push_back(ContentScreenRect{.x = position.x, .y = position.y, .width = size.x, .height = size.y});
    if (visible) {
        // Changes wait for the end of the draw: they rebuild the rows.
        const std::vector<ContentFolderRow>& rows = m_model->treeRows();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        // A rename that starts on a tree row draws its row, scrolled off or not, so the field opens.
        if (m_renameFocus && m_renameItem.has_value() && m_renameItem->id.isNil() &&
            !m_model->shownIndexOf(*m_renameItem).has_value()) {
            const auto row = std::ranges::find(rows, m_renameItem->path, &ContentFolderRow::path);
            if (row != rows.end()) {
                clipper.IncludeItemByIndex(static_cast<int>(row - rows.begin()));
            }
        }
        while (clipper.Step()) {
            for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                drawTreeRow(rows[static_cast<std::size_t>(index)]);
            }
        }
    }
    ImGui::EndChild();
}

void ContentBrowserPanel::drawTreeRow(const ContentFolderRow& row) {
    ImGui::PushID(row.path.c_str());
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + static_cast<float>(row.depth) * ImGui::GetStyle().IndentSpacing);
    const ContentKey key{.id = AssetId(), .path = row.path};
    // A folder the asset view doesn't show is renamed here.
    const bool rename = renaming(key) && !m_model->shownIndexOf(key).has_value();
    // NOLINTBEGIN(bugprone-signed-bitwise): ImGui's flags are int enums
    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (!row.hasChildren) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (row.path == m_model->currentFolder()) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (rename) {
        flags |= ImGuiTreeNodeFlags_AllowOverlap;
    }
    // NOLINTEND(bugprone-signed-bitwise)
    if (row.hasChildren) {
        ImGui::SetNextItemOpen(row.expanded);
    }
    const std::string label = iconText(icons::kFolder) + (rename ? std::string() : " " + row.name);
    const bool open = ImGui::TreeNodeEx("##folder", flags, "%s", label.c_str());
    m_folderRects[row.path] = visibleRectOfItem();
    if (ImGui::IsItemToggledOpen()) {
        m_actions.emplace_back([this, path = row.path, expand = open] { m_model->setExpanded(path, expand); });
    } else if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        m_actions.emplace_back([this, path = row.path, expand = !row.expanded] { m_model->setExpanded(path, expand); });
    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && row.path != m_model->currentFolder()) {
        m_actions.emplace_back(
            [this, path = row.path] { runCommand("ith.content.navigate", stringArgs("folder", path)); });
    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        m_openMenu = Menu::TreeFolder;
        m_menuFolder = row.path;
    }
    dragAndDrop(key, nullptr);
    dropTarget(row.path);
    if (rename) {
        if (m_renameFocus) {
            ImGui::SetScrollHereY(); // the field opens in view
        }
        ImGui::SameLine();
        drawRenameField(std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 4.0f));
    }
    ImGui::PopID();
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
        m_segments.push_back(DropTarget{.folder = target, .rect = visibleRectOfItem()});
        if (pressed && target != m_model->currentFolder()) {
            m_actions.emplace_back(
                [this, target] { runCommand("ith.content.navigate", stringArgs("folder", target)); });
        }
        dropTarget(target);
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

bool ContentBrowserPanel::renaming(const ContentKey& key) const {
    return m_renameItem.has_value() && *m_renameItem == key;
}

void ContentBrowserPanel::drawRenameField(float width) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float extension =
        m_renameExtension.empty() ? 0.0f : ImGui::CalcTextSize(m_renameExtension.c_str()).x + style.ItemSpacing.x;
    if (m_renameFocus) {
        ImGui::SetKeyboardFocusHere();
        m_renameFocus = false;
    }
    ImGui::SetNextItemWidth(std::max(width - extension, ImGui::GetFontSize() * 2.0f));
    const bool enter = ImGui::InputText("##rename", m_renameText.data(), m_renameText.size(), kRenameField);
    recordControl("rename");
    if (ImGui::IsItemEdited()) {
        m_renameError.clear();
    }
    const bool deactivated = ImGui::IsItemDeactivated();
    const ImVec2 below(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + style.ItemSpacing.y);
    if (!m_renameExtension.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_renameExtension.c_str());
    }
    // Checked as the user types, with the extension (spec 3.8); a refusal of the command stays until
    // the next edit.
    const std::string name(m_renameText.data());
    std::string reason;
    if (name.empty()) {
        reason = "a name can't be empty";
    } else if (const Expected<void> valid = checkContentName(name + m_renameExtension); !valid) {
        reason = valid.error().message;
    }
    const std::string& shown = reason.empty() ? m_renameError : reason;
    if (!shown.empty()) {
        ImGui::SetNextWindowPos(below);
        if (ImGui::BeginTooltip()) {
            const std::string text = iconText(icons::kWarning) + " " + shown;
            ImGui::TextUnformatted(text.c_str());
            ImGui::EndTooltip();
        }
    }
    if (!m_renameItem.has_value()) {
        return;
    }
    const ContentKey item = *m_renameItem;
    // Enter commits and keeps the field open on a refusal; leaving the field commits a valid name and
    // drops an invalid one; Escape drops it.
    const bool escape = deactivated && !enter && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (escape || (deactivated && !enter && !reason.empty())) {
        m_actions.emplace_back([this] { runCommand("ith.content.dismiss", Value::object()); });
        return;
    }
    if (enter && !reason.empty()) {
        m_renameFocus = true;
        return;
    }
    if (!enter && !deactivated) {
        return;
    }
    m_actions.emplace_back([this, item, name, keepOpen = enter] {
        Value args = itemArgs(item);
        args.set("name", Value::string(name));
        if (runCommand("ith.content.rename", args).has_value()) {
            return;
        }
        m_renameError = m_notice;
        if (keepOpen) {
            m_renameFocus = true;
        } else {
            runCommand("ith.content.dismiss", Value::object());
        }
    });
}

void ContentBrowserPanel::itemInput(const ContentItem& item) {
    const ContentKey key = keyOf(item);
    m_itemRects[key] = visibleRectOfItem();
    ++m_itemsDrawn;
    const bool selected = m_model->isSelected(key);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        m_actions.emplace_back([this, key] { open(key); });
    } else if (selectsOnClick(selected && m_model->selection().size() > 1)) {
        m_actions.emplace_back(
            [this, key, mode = std::string(clickMode())] { runCommand("ith.content.select", selectArgs(key, mode)); });
    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        if (!selected) {
            m_actions.emplace_back([this, key] { runCommand("ith.content.select", selectArgs(key, "replace")); });
        }
        m_openMenu = Menu::Items;
    }
    if (!renaming(key) && ImGui::GetDragDropPayload() == nullptr) {
        drawTooltip(item, m_model->searching());
    }
    dragAndDrop(key, &item);
}

void ContentBrowserPanel::dragAndDrop(const ContentKey& key, const ContentItem* item) {
    if (ImGui::BeginDragDropSource()) {
        const ImGuiPayload* current = ImGui::GetDragDropPayload();
        if (current == nullptr || !current->IsDataType(kItemsPayload) || m_dragged.empty()) {
            // Dragging a selected item drags the selection; an unselected one is selected alone first
            // (spec 3.9). A tree folder is dragged alone.
            if (item != nullptr && m_model->isSelected(key)) {
                m_dragged = m_model->selection();
            } else {
                m_dragged = {key};
                if (item != nullptr) {
                    m_actions.emplace_back(
                        [this, key] { runCommand("ith.content.select", selectArgs(key, "replace")); });
                }
            }
        }
        ImGui::SetDragDropPayload(kItemsPayload, nullptr, 0);
        if (!m_dragRefusal.empty()) {
            const std::string text = iconText(icons::kWarning) + " " + m_dragRefusal;
            ImGui::TextUnformatted(text.c_str());
        } else if (m_dragged.size() == 1) {
            const ContentItem* dragged = m_model->find(m_dragged.front());
            const std::string text = "Move " + (dragged != nullptr ? dragged->label : std::string("Content"));
            ImGui::TextUnformatted(text.c_str());
        } else {
            ImGui::Text("Move %zu items", m_dragged.size());
        }
        ImGui::EndDragDropSource();
    }
    if (item != nullptr && item->kind == ContentItemKind::Folder) {
        dropTarget(item->path);
    }
}

std::string ContentBrowserPanel::refusal(std::string_view folder) const {
    for (const ContentKey& key : m_dragged) {
        if (key.id.isNil() && key.path.empty()) {
            return "The Content folder can't be moved";
        }
        const ContentItem* item = m_model->find(key);
        if (item == nullptr) {
            continue;
        }
        if (item->kind == ContentItemKind::Problem) {
            return "'" + item->label + "' is a problem entry, which can't be moved: fix, rename or delete it";
        }
        if (item->kind == ContentItemKind::Folder &&
            (folder == item->path || (folder.size() > item->path.size() && folder.starts_with(item->path) &&
                                      folder[item->path.size()] == '/'))) {
            return "'" + item->label + "' can't move into itself or a folder below it";
        }
    }
    return {};
}

void ContentBrowserPanel::dropTarget(const std::string& folder) {
    if (!ImGui::BeginDragDropTarget()) {
        return;
    }
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    if (payload != nullptr && payload->IsDataType(kItemsPayload)) {
        if (std::string why = refusal(folder); !why.empty()) {
            m_frameRefusal = std::move(why); // no highlight, a "not allowed" cursor and the reason
        } else {
            m_dragOverTarget = true;
            if (const ImGuiPayload* delivered = ImGui::AcceptDragDropPayload(kItemsPayload);
                delivered != nullptr && delivered->IsDelivery()) {
                Value args = Value::object();
                args.set("items", keyList(m_dragged));
                args.set("folder", Value::string(folder));
                m_actions.emplace_back([this, args = std::move(args)] { runCommand("ith.content.move", args); });
            }
        }
    }
    ImGui::EndDragDropTarget();
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
        ImGui::TextDisabled("%s", m_model->searching() ? "No items match"
                                                       : "This folder is empty. Drop files here or use Import.");
    } else if (m_model->view().mode == ContentViewMode::Grid) {
        drawGrid();
    } else {
        m_columns = 1;
        drawList();
    }
    keyboardInput();
    // A click on the empty space between and below the items clears the selection; a right-click
    // there opens the menu of the folder.
    const bool empty = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::IsAnyItemHovered();
    if (empty && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_model->selection().empty()) {
        Value args = Value::object();
        args.set("items", Value::array());
        runCommand("ith.content.select", args);
    }
    if (empty && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        m_openMenu = Menu::EmptySpace;
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
    const bool rename = renaming(keyOf(item));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    // The rename field lies over the tile: the group keeps the grid's layout, and the tile lets the
    // field take the mouse.
    if (rename) {
        ImGui::BeginGroup();
        ImGui::SetNextItemAllowOverlap();
    }
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
    const float labelY = min.y + kTileIconHeight * scale + style.FramePadding.y;
    if (!rename) {
        const auto [first, second] = tileLines(item.label, size.x - style.FramePadding.x * 2.0f);
        float y = labelY;
        for (const std::string* line : {&first, &second}) {
            if (!line->empty()) {
                const float lineWidth = ImGui::CalcTextSize(line->c_str()).x;
                draw->AddText(ImVec2(min.x + (size.x - lineWidth) * 0.5f, y), color, line->c_str());
                y += lineHeight;
            }
        }
    }
    itemInput(item);
    if (rename) {
        ImGui::SetCursorScreenPos(ImVec2(min.x, labelY));
        drawRenameField(size.x);
        ImGui::EndGroup();
    }
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
    const bool rename = renaming(key);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    // The row is an unlabelled selectable with the label drawn after it: ImGui would hide a "##" in a
    // name.
    const float start = ImGui::GetCursorPosX();
    if (rename) {
        ImGui::SetNextItemAllowOverlap();
    }
    ImGui::Selectable("##row", m_model->isSelected(key), kListRow);
    if (m_scrollToFocus && m_model->focusItem() == key) {
        ImGui::SetScrollHereY();
        m_scrollToFocus = false;
    }
    itemInput(item);
    ImGui::SameLine(start);
    if (rename) {
        ImGui::TextUnformatted(iconText(iconOf(item)).c_str());
        ImGui::SameLine();
        drawRenameField(ImGui::GetContentRegionAvail().x);
    } else {
        const std::string label = iconText(iconOf(item)) + " " + item.label +
                                  (item.hasProblems() ? "  " + iconText(icons::kWarning) : std::string());
        if (item.kind == ContentItemKind::Problem) {
            ImGui::TextDisabled("%s", label.c_str());
        } else {
            ImGui::TextUnformatted(label.c_str());
        }
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
    if (!m_notice.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::TextUnformatted(m_notice.c_str());
    }
}

// --- menus and dialogs --------------------------------------------------------------------------

void ContentBrowserPanel::drawContextMenu() {
    if (m_openMenu != Menu::None) {
        m_menu = m_openMenu;
        m_openMenu = Menu::None;
        ImGui::OpenPopup(kMenuPopup);
    }
    if (!ImGui::BeginPopup(kMenuPopup)) {
        return;
    }
    const auto entry = [this](const char* label, bool enabled, std::function<void()> action) {
        if (ImGui::MenuItem(label, nullptr, false, enabled)) {
            m_actions.push_back(std::move(action));
        }
        recordControl(std::string("menu:") + label);
    };
    switch (m_menu) {
        case Menu::Items:
            drawItemsMenu();
            break;
        case Menu::TreeFolder: {
            const std::string folder = m_menuFolder;
            Value parent = Value::object();
            parent.set("parent", Value::string(folder));
            entry("New Folder", true, [this, parent] { runCommand("ith.content.new_folder", parent); });
            const ContentKey key{.id = AssetId(), .path = folder};
            entry("Rename", !folder.empty(), [this, key] { runCommand("ith.content.rename", itemArgs(key)); });
            Value items = Value::object();
            items.set("items", keyList({key}));
            entry("Delete", !folder.empty(), [this, items] { runCommand("ith.content.delete", items); });
            break;
        }
        case Menu::EmptySpace:
            entry("New Folder", true, [this] { runCommand("ith.content.new_folder", Value::object()); });
            entry("Import...", true, [this] { showImportDialog(); });
            break;
        case Menu::None:
            break;
    }
    ImGui::EndPopup();
}

void ContentBrowserPanel::drawItemsMenu() {
    const ContentKey* primary = m_model->primary();
    const ContentItem* item = primary != nullptr ? m_model->find(*primary) : nullptr;
    bool anyAsset = false;
    bool anyImported = false;
    for (const ContentKey& key : m_model->selection()) {
        const ContentItem* selected = m_model->find(key);
        if (selected == nullptr || selected->kind != ContentItemKind::Asset) {
            continue;
        }
        anyAsset = true;
        const AssetRecord* record = m_model->assets()->find(selected->id);
        anyImported = anyImported || (record != nullptr && record->imported);
    }
    const bool sidecar = item != nullptr && item->kind == ContentItemKind::Problem &&
                         endsWithIgnoreCaseAscii(item->path, kSidecarSuffix);
    const auto entry = [this](const char* label, bool enabled, std::string_view id, Value args) {
        if (ImGui::MenuItem(label, nullptr, false, enabled)) {
            m_actions.emplace_back([this, id, args = std::move(args)] { runCommand(id, args); });
        }
        recordControl(std::string("menu:") + label);
    };
    const Value primaryArgs = item != nullptr ? itemArgs(keyOf(*item)) : Value::object();
    entry("Rename", item != nullptr && !sidecar, "ith.content.rename", primaryArgs);
    entry("Duplicate", anyAsset, "ith.content.duplicate", Value::object());
    entry("Delete", !m_model->selection().empty(), "ith.content.delete", Value::object());
    // Greyed from the selection (spec 3.13): only imported files can be reimported.
    entry("Reimport", anyImported, "ith.content.reimport", Value::object());
    if (item != nullptr && item->kind == ContentItemKind::Problem) {
        const auto has = [item](AssetProblemKind kind) {
            return std::ranges::any_of(item->problems,
                                       [kind](const ContentBadge& badge) { return badge.kind == kind; });
        };
        if (has(AssetProblemKind::MissingSidecar)) {
            ImGui::Separator();
            entry("Import", true, "ith.content.fix", primaryArgs);
        } else if (has(AssetProblemKind::DuplicateId)) {
            ImGui::Separator();
            entry("Assign New Id", true, "ith.content.fix", primaryArgs);
        }
    }
}

void ContentBrowserPanel::drawPromptDialogs() {
    const ContentPrompt* prompt = m_model->prompt();
    // While an import waits for its frame, its conflict dialog is answered already.
    const bool waiting = m_pendingImport.has_value();
    const bool wantDelete = prompt != nullptr && prompt->kind == ContentPromptKind::Delete && !waiting;
    const bool wantConflicts = prompt != nullptr && prompt->kind == ContentPromptKind::ImportConflicts && !waiting;
    // What the buttons asked for runs after the dialog: a command replaces the prompt drawn from.
    std::vector<std::function<void()>> after;

    const Value* preview = wantDelete ? prompt->details.find("preview") : nullptr;
    const std::uint64_t count = preview != nullptr ? numberOf(*preview, "count") : 0;
    const std::string deleteTitle = "Delete " + counted(count, "item") + "?" + kDeletePopupId;
    if (wantDelete && !ImGui::IsPopupOpen(kDeletePopupId)) {
        ImGui::OpenPopup(kDeletePopupId);
    }
    if (ImGui::BeginPopupModal(deleteTitle.c_str(), nullptr, kDialog)) {
        if (wantDelete) {
            m_frameDialog = ContentDialog::Delete;
            drawDeleteDialog(prompt->details, after);
        } else {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (wantConflicts && !ImGui::IsPopupOpen(kConflictPopup)) {
        ImGui::OpenPopup(kConflictPopup);
        m_conflict = 0;
        m_choices = Value::object();
        m_applyToAll = false;
    }
    if (ImGui::BeginPopupModal(kConflictPopup, nullptr, kDialog)) {
        if (wantConflicts) {
            m_frameDialog = ContentDialog::ImportConflicts;
            drawConflictDialog(prompt->details, after);
        } else {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    for (const std::function<void()>& action : after) {
        action();
    }
}

void ContentBrowserPanel::drawDeleteDialog(const Value& details, std::vector<std::function<void()>>& after) {
    const Value* found = details.find("preview");
    const Value& preview = found != nullptr ? *found : details;
    const std::vector<Value>& items = listOf(preview, "items");
    const std::vector<Value>& uses = listOf(preview, "uses");
    const std::vector<Value>& unreadable = listOf(preview, "unreadable");
    const float width = ImGui::GetFontSize() * 36.0f;
    // The lists scroll when they are long; the warning and the buttons stay in view.
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, ImGui::GetFontSize() * 20.0f));
    if (ImGui::BeginChild("##delete_list", ImVec2(width, 0.0f), kListChild)) {
        drawDeleteItems(items);
        drawUses(uses, width);
        drawUnreadable(unreadable);
    }
    ImGui::EndChild();
    ImGui::TextUnformatted("This can't be undone. The files are deleted from disk.");
    ImGui::Separator();
    if (ImGui::Button("Delete")) {
        Value args = Value::object();
        args.set("items", Value::array(items));
        args.set("confirmed", Value::boolean(true));
        after.emplace_back([this, args = std::move(args)] { runCommand("ith.content.delete", args); });
    }
    recordControl("Delete");
    ImGui::SameLine();
    // Cancel has the keyboard focus, so Enter cancels too (spec 3.11); Tab moves it to Delete.
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    bool cancel = ImGui::Button("Cancel");
    recordControl("Cancel");
    const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    cancel = cancel || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || (enter && ImGui::IsItemFocused());
    if (cancel) {
        after.emplace_back([this] { runCommand("ith.content.dismiss", Value::object()); });
    }
}

void ContentBrowserPanel::drawConflictDialog(const Value& details, std::vector<std::function<void()>>& after) {
    const std::vector<Value>& conflicts = listOf(details, "conflicts");
    if (conflicts.empty()) {
        return;
    }
    m_conflict = std::min(m_conflict, conflicts.size() - 1);
    const Value& conflict = conflicts[m_conflict];
    const std::string file(textOf(conflict, "file"));
    const bool canReplace = flagOf(conflict, "replace");
    ImGui::Text("File %zu of %zu", m_conflict + 1, conflicts.size());
    const std::string question = "'" + std::string(fs::fileName(fs::normalizePath(file))) + "': '" +
                                 std::string(textOf(conflict, "target")) + "' is in Content already.";
    ImGui::TextUnformatted(question.c_str());
    ImGui::TextDisabled("%s", file.c_str());
    const std::size_t remaining = conflicts.size() - m_conflict - 1;
    if (remaining > 0) {
        const std::string label = "Apply to all remaining (" + std::to_string(remaining) + ")###apply";
        ImGui::Checkbox(label.c_str(), &m_applyToAll);
        recordControl("Apply to all remaining");
    }
    ImGui::Separator();
    std::optional<std::string_view> choice;
    ImGui::BeginDisabled(!canReplace);
    if (ImGui::Button("Replace")) {
        choice = "replace";
    }
    ImGui::EndDisabled();
    recordControl("Replace");
    // NOLINTNEXTLINE(bugprone-signed-bitwise): ImGui's flags are int enums
    if (!canReplace && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Only an imported file of the same type can be replaced");
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep Both")) {
        choice = "keep_both";
    }
    recordControl("Keep Both");
    ImGui::SetItemTooltip("Import it under the next free name");
    ImGui::SameLine();
    if (ImGui::Button("Skip")) {
        choice = "skip";
    }
    recordControl("Skip");
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel");
    recordControl("Cancel");
    if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        // Nothing is imported (spec 3.12).
        after.emplace_back([this] { runCommand("ith.content.dismiss", Value::object()); });
        return;
    }
    if (!choice.has_value()) {
        return;
    }
    // The answer goes to this file, or with "Apply to all remaining" to the rest too; each conflict's
    // "file" as reported is its key in "choices".
    const std::size_t last = m_applyToAll ? conflicts.size() : m_conflict + 1;
    for (std::size_t i = m_conflict; i < last; ++i) {
        m_choices.set(std::string(textOf(conflicts[i], "file")), Value::string(std::string(*choice)));
    }
    m_conflict = last;
    m_applyToAll = false;
    if (m_conflict < conflicts.size()) {
        return;
    }
    Value args = Value::object();
    const Value* files = details.find("files");
    args.set("files", files != nullptr ? *files : Value::array());
    args.set("folder", Value::string(std::string(textOf(details, "folder"))));
    args.set("choices", m_choices);
    after.emplace_back([this, args = std::move(args)]() mutable { queueImport(std::move(args)); });
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
        Value args = itemArgs(*m_pendingOpen);
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

void ContentBrowserPanel::drawDropHighlight() {
    m_dropHighlight.reset();
    if (!m_osDrag.has_value() || questionOpen()) { // a drop there would be refused
        return;
    }
    const std::optional<DropTarget> target = dropTargetAt(m_osDrag->first, m_osDrag->second);
    if (!target.has_value()) {
        return;
    }
    m_dropHighlight = target->rect;
    const UiRect& rect = target->rect;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImVec2 min(rect.x, rect.y);
    const ImVec2 max(rect.x + rect.width, rect.y + rect.height);
    const ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_DragDropTarget);
    draw->AddRectFilled(min, max, ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, color.w * 0.15f)));
    draw->AddRect(min, max, ImGui::GetColorU32(color), 0.0f, 2.0f * displayScale());
}

void ContentBrowserPanel::drawDrawer() {
    runPendingImport();
    if (m_pendingOpen.has_value() || ImGui::IsPopupOpen(kUnsavedPopup)) {
        drawUnsavedQuestion();
    }
    drawPromptDialogs();
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
    m_model->setTreeFocus(m_treeFocused);
    m_treeFocused = false;
    // Where the browser was drawn this frame, for the presses and drops before the next frame.
    std::vector<ContentScreenRect> rects;
    m_browserRects.clear();
    if (m_panelRect.has_value()) {
        rects.push_back(*m_panelRect);
        m_browserRects.push_back(UiRect{
            .x = m_panelRect->x, .y = m_panelRect->y, .width = m_panelRect->width, .height = m_panelRect->height});
    }
    if (m_drawerRect.has_value()) {
        rects.push_back(ContentScreenRect{
            .x = m_drawerRect->x, .y = m_drawerRect->y, .width = m_drawerRect->width, .height = m_drawerRect->height});
        m_browserRects.push_back(*m_drawerRect);
    }
    m_model->setScreenRects(std::move(rects), std::move(m_treeRects));
    m_treeRects.clear(); // valid but unspecified after the move
    m_panelRect.reset();
    m_panelFocused = false;
    drawDropHighlight();
    // A drag between the browser's widgets: the reason a target refused it shows in its tooltip, and
    // anywhere but a target that takes it the cursor says "not allowed" (spec 3.9).
    if (const ImGuiPayload* payload = ImGui::GetDragDropPayload();
        payload != nullptr && payload->IsDataType(kItemsPayload)) {
        m_dragRefusal = std::move(m_frameRefusal);
        if (!m_dragOverTarget) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
        }
    } else {
        m_dragged.clear();
        m_dragRefusal.clear();
    }
    m_frameRefusal.clear();
    m_dragOverTarget = false;
    m_controls = std::move(m_frameControls);
    m_frameControls.clear();
    m_dialog = m_frameDialog;
    m_frameDialog = ContentDialog::None;
}

void registerContentBrowser(EditorSession& session, ContentBrowserPanel& panel) {
    session.panels().add({.id = std::string(kContentBrowserPanelId),
                          .displayName = "Content Browser",
                          .defaultDock = DockPlace::Bottom,
                          .draw = [&panel](std::string_view /*instance*/) { panel.draw(); }});
    session.commands().setPrompt("ith.content.open", [&panel](EditorSession&) { return panel.promptOpen(); });
}

} // namespace ith
