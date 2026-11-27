#pragma once

// ith::ContentBrowserModel: the Content Browser's view of the project's Content folder, as plain
// data (ADR-0049, docs/specs/content-browser.md); editor_ui draws it in the panel and the content
// drawer, and the ith.content commands change it.
//
//   ith::ContentBrowserModel& content = session.content();
//   session.execute("ith.content.rescan");                  // scan the disk, build the items
//   session.execute("ith.content.navigate", folderArgs);    // {"folder": "Meshes"}
//   for (const ith::ContentItem* item : content.shown()) { ... draw item->label ... }
//
// What it holds:
// - the folders of the asset database (AssetDatabase::folders) as a tree with expansion, and the
//   current folder (the root is the empty path "");
// - the items: every folder, every asset of the database and a problem entry for each file on disk
//   that the last scan found something wrong with (spec 3.2), with their problem badges;
// - the shown items (shown()), in display order: the current folder's own items, or, while a search
//   or type filter is active, the matching assets and problem entries of the current folder and
//   every folder below it (ADR-0049 rule 18). Folders come first; names sort in natural order
//   (naturalCompare);
// - the content selection with its anchor and focus item, the view settings, the drawer's open
//   state, the keyboard-focus flag editor_ui reports every frame, and the session state.
//
// Nothing is computed per frame: every change rebuilds what it affects at once and bumps
// generation(), which panels poll (ADR-0037 rule 13). The items are rebuilt from the database by
// refresh() (rescan() scans first); the database is never watched, so outside changes wait for a
// Rescan (ADR-0035 rule 9). Nothing here is undoable or touches the level (ADR-0049 rule 2).
//
// Session state (ADR-0049 rule 19): the current folder, the view mode, the sort, the expanded
// folders, the tree's width and the drawer's height are kept in <project>/Saved/Editor/
// ContentBrowser.json, an ith.content_browser_state document, version 1, written atomically (not
// durably: it can be rebuilt) whenever one of them changes, and read when the model is made. A
// missing file means the defaults, silently; an unreadable one means the defaults and one warning in
// the "ContentBrowser" log category. A saved folder that no longer exists falls back to its nearest
// existing ancestor at the first refresh. The search, the filter, the selection and whether the
// drawer is open are not kept.
//
// Main thread only. The database, when set, must outlive the model.

#include "ith/asset_database/AssetDatabase.h"
#include "ith/assets/AssetId.h"
#include "ith/foundation/Expected.h"
#include "ith/serialization/Document.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ith {

// The group a type is shown and filtered under (spec 3.2).
enum class ContentCategory : std::uint8_t {
    Level,
    Model,
    Texture,
    Audio,
    Data,
    Other,
};

inline constexpr std::size_t kContentCategoryCount = 6;
static_assert(static_cast<std::size_t>(ContentCategory::Other) + 1 == kContentCategoryCount);

// The category's display name: "Level", "Model", "Texture", "Audio", "Data" or "Other".
[[nodiscard]] std::string_view displayName(ContentCategory category) noexcept;

// The category's name in commands and queries: "level", "model", "texture", "audio", "data" or
// "other".
[[nodiscard]] std::string_view categoryId(ContentCategory category) noexcept;

// The category named `id` (categoryId), or nullopt.
[[nodiscard]] std::optional<ContentCategory> parseCategory(std::string_view id) noexcept;

// What the type category table says about an asset type.
struct ContentType {
    std::string_view displayName;
    ContentCategory category = ContentCategory::Other;
};

// The type category table (spec 3.2): an asset type (a native asset's format, an imported file's
// importer id) to its display name and category. A type the table doesn't know is Other, with the
// raw type as its display name (valid as long as `type` is). A module that brings a format or an
// importer adds its row here.
[[nodiscard]] ContentType contentTypeOf(std::string_view type) noexcept;

// Natural order (spec 3.3): labels split into runs of digits and runs of other bytes; other bytes
// compare without ASCII case, digit runs by value, equal values with the shorter run first ("Rock7"
// before "Rock07"). Returns < 0, 0 or > 0. Equal only for labels that differ just in ASCII case;
// callers break such ties by the bytes.
[[nodiscard]] int naturalCompare(std::string_view a, std::string_view b) noexcept;

enum class ContentItemKind : std::uint8_t {
    Folder,
    Asset,
    // A file on disk that is not in the database because something is wrong with it.
    Problem,
};

// A problem mark on an item, from the last scan (spec 3.2).
struct ContentBadge {
    AssetProblemKind kind = AssetProblemKind::InvalidFile;
    // What the badge's tooltip says: "Not imported", "Source file missing", "Duplicate id of
    // <path>", "Differs only in case from <path>", or the scan's message.
    std::string message;
    // The other path a duplicate id or a case conflict names; empty otherwise.
    std::string otherPath;

    friend bool operator==(const ContentBadge&, const ContentBadge&) = default;
};

// One item of the Content Browser.
struct ContentItem {
    ContentItemKind kind = ContentItemKind::Asset;
    // The content path: relative to Content, '/'-separated.
    std::string path;
    // The content path of the folder that holds it; "" for the root.
    std::string folder;
    // Assets only.
    AssetId id;
    // What it shows: a folder's name, a native asset's file name without ".ithasset" or ".ithmap",
    // an imported asset's or a problem entry's full file name.
    std::string label;
    // An asset's database type; empty for folders and problem entries.
    std::string type;
    // Assets: from the type category table; folders and problem entries: Other.
    ContentCategory category = ContentCategory::Other;
    std::vector<ContentBadge> problems;

    [[nodiscard]] bool hasProblems() const noexcept { return !problems.empty(); }
};

// How commands and the selection name an item (spec 3.6): an asset by its id (and an empty path),
// a folder or a problem entry by its content path (and a nil id), so a renamed asset stays selected.
struct ContentKey {
    AssetId id;
    std::string path;

    friend bool operator==(const ContentKey&, const ContentKey&) = default;
};

// The key of `item`.
[[nodiscard]] ContentKey keyOf(const ContentItem& item);

struct ContentKeyHash {
    [[nodiscard]] std::size_t operator()(const ContentKey& key) const noexcept {
        return std::hash<AssetId>()(key.id) ^ (std::hash<std::string>()(key.path) << 1U);
    }
};

enum class ContentViewMode : std::uint8_t {
    Grid,
    List,
};

enum class ContentSortKey : std::uint8_t {
    // Natural order of the labels.
    Name,
    // The category's display name, then the type's display name, then by name.
    Type,
};

// The view settings; session state.
struct ContentViewSettings {
    ContentViewMode mode = ContentViewMode::Grid;
    ContentSortKey sortBy = ContentSortKey::Name;
    bool descending = false;

    friend bool operator==(const ContentViewSettings&, const ContentViewSettings&) = default;
};

// The type filter (spec 3.5): a set of categories, combined with OR, plus Problems (items with a
// badge, and problem entries). Empty means everything.
struct ContentFilter {
    // Bit i for ContentCategory i.
    std::uint8_t categories = 0;
    bool problems = false;

    [[nodiscard]] bool empty() const noexcept { return categories == 0 && !problems; }

    [[nodiscard]] bool has(ContentCategory category) const noexcept {
        return (categories & (1U << static_cast<unsigned>(category))) != 0;
    }

    void add(ContentCategory category) noexcept {
        categories = static_cast<std::uint8_t>(categories | (1U << static_cast<unsigned>(category)));
    }

    friend bool operator==(const ContentFilter&, const ContentFilter&) = default;
};

// How ith.content.select changes the selection (spec 3.6).
enum class ContentSelect : std::uint8_t {
    // A click: the items alone; the last becomes the anchor and the focus item. No items clears it.
    Replace,
    // The items added (as the last, so the last is primary); the last becomes anchor and focus.
    Add,
    // A Ctrl + click: each item in or out; the last becomes anchor and focus.
    Toggle,
    // The items taken out.
    Remove,
    // A Shift + click: the shown items from the anchor to the (one) item, replacing the selection;
    // the anchor stays and the item becomes the focus and the primary. Without an anchor, Replace.
    Range,
    // A Ctrl + Shift + click: Range added to the selection; items already selected keep their place.
    AddRange,
};

// One folder of the tree, in tree order (each folder, then its subfolders depth-first).
struct ContentFolderRow {
    // "" for the root, which is the first row.
    std::string path;
    // "Content" for the root.
    std::string name;
    // 0 for the root.
    std::uint32_t depth = 0;
    bool hasChildren = false;
    bool expanded = false;
};

// The session state file, relative to the project directory (ADR-0049 rule 19).
inline constexpr std::string_view kContentBrowserStateFile = "Saved/Editor/ContentBrowser.json";

inline constexpr serialization::DocumentFormat kContentBrowserStateFormat{
    .name = "ith.content_browser_state", .version = 1, .minimumVersion = 1};

// The largest session state file read.
inline constexpr std::size_t kMaxContentBrowserStateSize = std::size_t{4} << 20;

// The tree's share of the browser's width at first (spec 3.1).
inline constexpr float kDefaultContentTreeWidth = 0.2f;

// The longest search text kept, in bytes; a longer one is cut at the last whole UTF-8 character
// that fits.
inline constexpr std::size_t kMaxContentSearchSize = 255;

// The session state, as the state file holds it.
struct ContentBrowserState {
    std::string folder;
    ContentViewSettings view;
    std::vector<std::string> expanded;
    float treeWidth = kDefaultContentTreeWidth;
    float drawerHeight = 0.0f;
};

// Reads a session state file's text (`source` names it in errors): an ith.content_browser_state
// document whose data may hold "folder", "view" ("grid" or "list"), "sortBy" ("name" or "type"),
// "descending", "expanded" (folder paths; other elements are skipped), "treeWidth" (clamped to
// [0.05, 0.95]) and "drawerHeight" (negative becomes 0); missing members keep their defaults. Fails
// with the document's errors, and with "editor.content_browser.invalid_state" when the data is not an
// object or a member has another type.
[[nodiscard]] Expected<ContentBrowserState> parseContentBrowserState(std::string_view text, std::string_view source);

// A rectangle in UI coordinates (ImGui's), where the browser was drawn.
struct ContentScreenRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    [[nodiscard]] bool contains(float px, float py) const noexcept {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};

class ContentBrowserModel {
public:
    // `assets` is the project's database (null: no project, and every content command fails with
    // "editor.document.no_asset_database"); `stateFile` the session state file's path (empty: no
    // file is read or written). Reads the file. Nothing is shown until refresh() or rescan().
    ContentBrowserModel(AssetDatabase* assets, std::string stateFile);

    ContentBrowserModel(const ContentBrowserModel&) = delete;
    ContentBrowserModel& operator=(const ContentBrowserModel&) = delete;
    ContentBrowserModel(ContentBrowserModel&&) = delete;
    ContentBrowserModel& operator=(ContentBrowserModel&&) = delete;
    ~ContentBrowserModel() = default;

    // The database; null without a project.
    [[nodiscard]] AssetDatabase* assets() noexcept { return m_assets; }

    [[nodiscard]] const AssetDatabase* assets() const noexcept { return m_assets; }

    // Scans the disk (AssetDatabase::scan), replaces the problem list, logs the duplicate ids the
    // scan resolved, and refresh()es. Fails with "editor.document.no_asset_database" and the scan's
    // error, with nothing changed. Precondition: no save of the open level is being written (a save
    // may be about to add the level's file); the model can't see the document, so the callers check it:
    // ith.content.rescan refuses, and the session rescans only after a save has finished.
    Expected<void> rescan();

    // Rebuilds the items from the database and the problem list: the current folder becomes its
    // nearest existing ancestor when it is gone, expanded folders that are gone are forgotten, and
    // the selection keeps the items that still exist (assets by id) and are shown.
    void refresh();

    // The problems of the last scan, sorted by path.
    [[nodiscard]] const std::vector<AssetProblem>& problems() const noexcept { return m_problems; }

    // Every item, in no particular order. Valid until the next refresh.
    [[nodiscard]] const std::vector<ContentItem>& items() const noexcept { return m_items; }

    // The item with `key`, or null. Valid until the next refresh.
    [[nodiscard]] const ContentItem* find(const ContentKey& key) const;

    // The item at the content path `path`, or null.
    [[nodiscard]] const ContentItem* findByPath(std::string_view path) const;

    // True for "" and every folder of the database.
    [[nodiscard]] bool folderExists(std::string_view path) const;

    // The current folder; "" (the root) at first.
    [[nodiscard]] const std::string& currentFolder() const noexcept { return m_folder; }

    // Makes `folder` current, expands its ancestors, and clears the selection and the search; the
    // type filter stays. Fails with "editor.content.not_found" when it is not a folder.
    Expected<void> navigate(std::string_view folder);

    // navigate() to the current folder's parent; false at the root, where nothing changes.
    bool navigateUp();

    // The folder tree: the root, then every folder whose ancestors are all expanded. Rebuilt only
    // when the folders or the expansion change.
    [[nodiscard]] const std::vector<ContentFolderRow>& treeRows() const noexcept { return m_treeRows; }

    // Whether the tree shows the subfolders of `folder`; the root always does.
    [[nodiscard]] bool isExpanded(std::string_view folder) const;

    // Expands or collapses `folder` in the tree (session state). Unknown folders are ignored.
    void setExpanded(std::string_view folder, bool expanded);

    // The search text and the type filter (spec 3.5). Changing either rebuilds the shown items and
    // takes the items that are no longer shown out of the selection. A search text longer than
    // kMaxContentSearchSize is cut at a UTF-8 character boundary.
    [[nodiscard]] const std::string& search() const noexcept { return m_search; }

    [[nodiscard]] const ContentFilter& filter() const noexcept { return m_filter; }

    void setSearch(std::string text);
    void setFilter(ContentFilter filter);

    // True while a search or a filter is active: shown() then lists matches from the whole subtree.
    [[nodiscard]] bool searching() const;

    // The shown items in display order. Valid until the next change.
    [[nodiscard]] const std::vector<const ContentItem*>& shown() const noexcept { return m_shown; }

    // The position of the item with `key` in shown(), or nullopt.
    [[nodiscard]] std::optional<std::size_t> shownIndexOf(const ContentKey& key) const;

    // How many shown items have a badge or are problem entries.
    [[nodiscard]] std::size_t shownProblems() const noexcept { return m_shownProblems; }

    [[nodiscard]] const ContentViewSettings& view() const noexcept { return m_view; }

    // Changes the view mode and the sort (session state) and re-sorts.
    void setView(const ContentViewSettings& view);

    // Changes the selection (spec 3.6). Fails with "editor.content.not_found", changing nothing, when
    // an item is not shown. Never touches the level's entity selection.
    Expected<void> select(std::span<const ContentKey> items, ContentSelect how);

    // Selects every shown item, in display order (the last becomes primary).
    void selectAll();

    // Clears the selection, the anchor and the focus item.
    void clearSelection();

    // The selected items in the order they were added; the last is the primary item.
    [[nodiscard]] const std::vector<ContentKey>& selection() const noexcept { return m_selection; }

    [[nodiscard]] bool isSelected(const ContentKey& key) const { return m_selected.contains(key); }

    [[nodiscard]] const ContentKey* primary() const noexcept {
        return m_selection.empty() ? nullptr : &m_selection.back();
    }

    [[nodiscard]] const std::optional<ContentKey>& anchor() const noexcept { return m_anchor; }

    [[nodiscard]] const std::optional<ContentKey>& focusItem() const noexcept { return m_focus; }

    // Whether the content drawer is open (not session state: it starts closed).
    [[nodiscard]] bool drawerOpen() const noexcept { return m_drawerOpen; }

    void setDrawerOpen(bool open);

    // Whether the browser (panel or drawer, with its child windows) has the keyboard focus; the
    // keymap reads it (ADR-0049 rule 4). editor_ui sets it at the end of every UI frame; between frames
    // a mouse press updates it (pointerPressed), and ith.content.toggle_drawer sets it, so keys that
    // follow in the same batch of events go to the right context.
    [[nodiscard]] bool keyboardFocus() const noexcept { return m_keyboardFocus; }

    void setKeyboardFocus(bool focused) noexcept { m_keyboardFocus = focused; }

    // Where the browser was drawn in the UI's last frame (the panel, the drawer, or neither), as
    // editor_ui reports it every frame.
    void setScreenRects(std::vector<ContentScreenRect> rects) { m_screenRects = std::move(rects); }

    // A mouse press at (`x`, `y`) in UI coordinates, seen by the UI's input consumer before the
    // events after it are routed: the browser has the keyboard focus when the press is inside one of
    // the last frame's rectangles, and loses it otherwise.
    void pointerPressed(float x, float y) noexcept;

    // The folder tree's share of the browser's width, in (0, 1); kDefaultContentTreeWidth at first.
    // Session state; values outside [0.05, 0.95] are clamped.
    [[nodiscard]] float treeWidth() const noexcept { return m_treeWidth; }

    void setTreeWidth(float fraction);

    // The drawer's height in pixels at display scale 1; 0 (at first) means 40 % of the window. The
    // UI clamps it to the window. Session state; negative values become 0.
    [[nodiscard]] float drawerHeight() const noexcept { return m_drawerHeight; }

    void setDrawerHeight(float height);

    // Changes whenever anything the browser shows changes; panels poll it.
    [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }

    // How many times the shown items were built, for tests of "nothing per frame".
    [[nodiscard]] std::uint64_t viewBuilds() const noexcept { return m_viewBuilds; }

private:
    void load();
    // The session state as the file's text; nullopt (logged) when it can't be written as JSON.
    [[nodiscard]] std::optional<std::string> stateText() const;
    void writeState();
    void rebuildTree();
    void rebuildView();
    // Takes keys that are not shown out of the selection, anchor and focus.
    void pruneSelection();
    void deselect(const ContentKey& key);
    // Adds `key` as the last, so it becomes the primary item.
    void appendSelected(const ContentKey& key);
    // Adds the shown items from index `from` to index `to` (both included) that aren't selected yet,
    // then makes the item at `to` the last.
    void selectRange(std::size_t from, std::size_t to);
    void sortShown();
    [[nodiscard]] bool matches(const ContentItem& item, std::span<const std::string> words) const;

    void changed() noexcept { ++m_generation; }

    AssetDatabase* m_assets;
    std::string m_stateFile;
    std::vector<AssetProblem> m_problems;
    std::vector<ContentItem> m_items;
    // Lowercase labels, parallel to m_items, for the search.
    std::vector<std::string> m_lowerLabels;
    // Each item's place in name order (naturalCompare, then the label's bytes, then the path),
    // parallel to m_items.
    std::vector<std::uint32_t> m_nameRanks;
    // Each item's place in type order (the category's display name, then the type's display name,
    // then name order), parallel to m_items.
    std::vector<std::uint32_t> m_typeRanks;
    std::unordered_map<std::string, std::size_t> m_byPath;
    std::unordered_map<AssetId, std::size_t> m_byId;
    // The items of each folder ("" for the root), by index into m_items.
    std::unordered_map<std::string, std::vector<std::size_t>> m_children;
    // "" (the root) always exists, also before the first refresh.
    std::unordered_set<std::string> m_folders{std::string()};
    std::vector<ContentFolderRow> m_treeRows;
    std::unordered_set<std::string> m_expanded;
    std::string m_folder;
    std::string m_search;
    ContentFilter m_filter;
    ContentViewSettings m_view;
    std::vector<const ContentItem*> m_shown;
    std::size_t m_shownProblems = 0;
    std::vector<ContentKey> m_selection;
    std::unordered_set<ContentKey, ContentKeyHash> m_selected;
    std::optional<ContentKey> m_anchor;
    std::optional<ContentKey> m_focus;
    bool m_drawerOpen = false;
    bool m_keyboardFocus = false;
    std::vector<ContentScreenRect> m_screenRects;
    float m_treeWidth = kDefaultContentTreeWidth;
    float m_drawerHeight = 0.0f;
    std::uint64_t m_generation = 0;
    std::uint64_t m_viewBuilds = 0;
    // What the state file holds, once read or written.
    std::optional<std::string> m_written;
    bool m_writeFailed = false;
};

} // namespace ith
