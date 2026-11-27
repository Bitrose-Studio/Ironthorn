#pragma once

// ith::ContentBrowserPanel: the Content Browser panel and the content drawer (ADR-0008 rules 2 and
// 4, ADR-0049 rules 4, 7 and 26, docs/specs/content-browser.md), drawn from editor_core's
// ContentBrowserModel; both show the same model.
//
//   ith::WindowUiHost host(platform, window);
//   ith::ContentBrowserPanel content(session, &host);
//   auto ui = ith::EditorUi::create({..., .host = &host, .content = &session.content(),
//                                    .contentBrowser = &content});
//   ith::registerContentBrowser(session, content); // the panel, and the prompt of ith.content.open
//   // every pumped event of the window (drops, the open-file dialog's results):
//   (*ui)->handleEvent(event);
//   // every frame, inside the UI frame:
//   ith::drawEditorShell(session, keymap, shell, dock);  // draws the panel when it is open
//   content.drawDrawer();                                // the drawer, the dialogs, the focus report
//
// The browser: a toolbar (Import, Add with New Folder, the search box, the type filter menu, grid or
// list, the sort menu, Rescan), the folder tree beside the path bar and the asset view, with a
// splitter between them, and a status line ("<n> items", "<m> selected", "<p> problems", a click on
// the problem count showing the Problems filter, then the last notice: the result of a file
// operation, why one was refused, or "Importing <n> files..." in the frame before an import). The
// tree and the asset view draw only their visible rows (list clippers); the list sorts by a click on
// its Name or Type header. A click selects (Ctrl toggles, Shift selects the range from the anchor,
// Ctrl + Shift adds it, a click on empty space clears; a plain click on an item of a larger selection
// selects it alone on the release, so a drag keeps the selection); a double-click opens: a folder
// becomes current, a level opens. The arrow keys, Home and End move the focus item and select
// (Shift: the range), Ctrl + F goes to the search box. Every change runs an ith.content command
// (ADR-0037); a refused one is logged in the "ContentBrowser" category and shown in the status line.
//
// The file operations (docs/specs/content-browser.md 3.7 to 3.13), drawn from the model's pending
// prompt (ContentBrowserModel::prompt), so a shortcut or a script that starts one shows it too:
// - the inline rename field (F2, Rename in a context menu, a new folder) over the item's label, or
//   on its tree row when the asset view doesn't show it: the editable part of the name with the
//   extension after it, checked as the user types (checkContentName, the reason shown under the
//   field). Enter runs ith.content.rename with the name and keeps the field open when the name is
//   invalid or the command refuses it; Escape runs ith.content.dismiss; leaving the field commits a
//   valid name and dismisses an invalid one;
// - the delete confirmation ("Delete <n> items?"): the items, at most 20, with the counts inside
//   folders, every used asset with its users (the open level, and whether only its unsaved changes
//   use it, included), the files that couldn't be read, and buttons Delete (ith.content.delete with
//   the previewed items and "confirmed") and Cancel, which has the keyboard focus;
// - the import conflicts, one file at a time: Replace (only where the conflict allows it), Keep
//   Both or Skip, with "Apply to all remaining"; the last answer runs ith.content.import again with
//   the answers in "choices", keyed by each conflict's "file" as reported.
// Cancel and Escape in a dialog run ith.content.dismiss.
//
// Context menus: on items Rename, Duplicate, Delete, Reimport (greyed unless the selection holds an
// imported asset) and the fix of a problem entry (Import for "Not imported", Assign New Id for a
// duplicate id); on a tree folder New Folder, Rename and Delete; on the asset view's empty space
// New Folder and Import. Items and tree folders are dragged onto folder items, tree folders and
// path bar segments (ith.content.move); a target that can't take the drag (a folder into itself or
// below it, a drag holding a problem entry or the Content folder) isn't highlighted, the cursor shows
// "not allowed" and the drag's tooltip says why.
//
// Import opens the platform's file dialog through the UiHost: several files, filtered by the
// extensions the registered importers read and the native asset extensions, with "All files",
// starting in the folder of the last import. The chosen files are imported into the folder that
// was current when Import was pressed. When no dialog can be shown, the error is logged and the
// status line says to drop files onto the browser instead. Files dragged from the operating system
// highlight the folder under the pointer (or the whole browser), and are imported into it, else the
// current folder, when dropped onto the panel or the drawer; drops elsewhere are ignored. Imports
// run in the frame after the one that drew the notice.
//
// The drawer is an ImGui window over the bottom of the main window, above the docked panels and the
// status bar, not dockable and in no layout. Its height starts at 40 % of the window, is dragged by
// its top edge and is clamped to 150 pixels (at display scale 1) and 80 % of the window. It closes on
// ith.content.toggle_drawer, on Escape with nothing selected (ith.content.dismiss), on a click
// outside it while it has no popup open, when a level opens from it, and when the panel is in the
// layout (one browser on screen at a time). Each frame drawDrawer() reports where the panel and the
// drawer were drawn, which the UI's input consumer hit-tests mouse presses against.
//
// Opening a level while the open one has unsaved changes asks Save / Don't Save / Cancel first and
// runs ith.content.open again with the answer.

#include "ith/editor_core/ContentBrowserModel.h"
#include "ith/editor_ui/EditorUi.h"
#include "ith/serialization/Value.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ith {

class EditorSession;

// Which dialog of a pending prompt the panel showed in the last frame.
enum class ContentDialog : std::uint8_t {
    None,
    Delete,
    ImportConflicts,
};

class ContentBrowserPanel {
public:
    // `session` must outlive the panel; so must `host`, which shows the open-file dialog (null: no
    // dialog can be shown, and Import says to drop files instead).
    explicit ContentBrowserPanel(EditorSession& session, UiHost* host = nullptr);

    ContentBrowserPanel(const ContentBrowserPanel&) = delete;
    ContentBrowserPanel& operator=(const ContentBrowserPanel&) = delete;
    ContentBrowserPanel(ContentBrowserPanel&&) = delete;
    ContentBrowserPanel& operator=(ContentBrowserPanel&&) = delete;
    // Removes the prompt of ith.content.open that registerContentBrowser set.
    ~ContentBrowserPanel();

    // Draws the panel's content inside its window. Main thread, inside a UI frame.
    void draw();

    // Draws the drawer when it is open, the unsaved-changes question when one is pending, the dialog
    // of a pending delete or import-conflict prompt and the highlight of an operating-system drag,
    // runs a queued import whose notice was drawn, then reports the browser's keyboard focus of this
    // frame (panel or drawer) and where it was drawn to the model. Call once per frame, after the
    // panels, also while the drawer is closed. Main thread, inside a UI frame.
    void drawDrawer();

    // Opens `item` as a double-click does: asks first when the open level has unsaved changes and
    // the item is a level; otherwise runs ith.content.open. True when it asked.
    bool open(const ContentKey& item);

    // The prompt of ith.content.open (Enter): open() on the primary item; false when nothing is
    // selected or nothing needs asking.
    bool promptOpen();

    // The entry points below (showImportDialog to dropEnded) run on the main thread and need the
    // EditorUi's ImGui context, between frames or inside one (a queued import records ImGui's frame
    // count). Files arriving while a question of the browser is open (the delete or conflict dialog,
    // the unsaved-changes question, a pending rename), or while another import is queued, are refused
    // with a notice in the status line and a warning in the log, never dropped silently.

    // What the Import button does: shows the platform's open-file dialog (UiHost::showOpenFileDialog)
    // for importing into the current folder. When it can't be shown (no host, a dialog already open,
    // or the host's error), logs the error and says in the status line to drop files instead; while a
    // question is open it is refused with a notice.
    void showImportDialog();

    // The open-file dialog's results, as EditorUi::handleEvent hands them over: a chosen file
    // (FileDialogFile), then the dialog's end (FileDialogClosed, with its error or empty), which
    // imports the chosen files into the folder that was current at showImportDialog(); an error is
    // handled as there. Ignored while no dialog of this panel is open. The next dialog starts in the
    // folder of the first chosen file, unless its name can't be used (fs::hasUnusableName).
    void dialogFileChosen(std::string_view path);
    void dialogClosed(std::string_view error);

    // An operating-system drag over the window, in UI coordinates, as EditorUi::handleEvent hands it
    // over: it entered (DropBegin), moved (DropPosition; the drop target is highlighted in the next
    // frames), dropped a file (DropFile, once per file) and ended (DropComplete). The end imports the
    // dropped files into dropFolderAt() the first file's position; a drop outside the browser, or
    // without files, does nothing. A drop with no DropPosition since DropBegin (Windows without its
    // drag and drop service reports files at 0, 0) goes into the current folder when the browser was
    // drawn in the last frame, and is refused with a notice otherwise.
    void dragEntered();
    void dragMoved(float x, float y);
    void fileDropped(std::string_view path, float x, float y);
    void dropEnded();

    // The folder an operating-system drop at (`x`, `y`) imports into, from where the browser was
    // drawn in the last frame: the folder item, tree folder or path bar segment under the point, else
    // the current folder; nullopt outside the panel and the drawer.
    [[nodiscard]] std::optional<std::string> dropFolderAt(float x, float y) const;

    // Where the last frame highlighted the target of an operating-system drag; nullopt when none.
    [[nodiscard]] std::optional<UiRect> dropHighlight() const { return m_dropHighlight; }

    // The status line's notice: the last file operation's result, why one was refused, or the
    // "Importing <n> files" line while an import waits for its frame; empty when there is none.
    [[nodiscard]] const std::string& notice() const noexcept { return m_notice; }

    // Whether an import waits to run in the next frame.
    [[nodiscard]] bool importPending() const noexcept { return m_pendingImport.has_value(); }

    // The dialog shown in the last frame.
    [[nodiscard]] ContentDialog dialog() const noexcept { return m_dialog; }

    // Where a labelled control of the toolbar or a dialog ("Import", "Add", "New Folder", "Delete",
    // "Cancel", "Replace", "Keep Both", "Skip", "Apply to all remaining"), an entry of a context menu
    // ("menu:" and its label: "menu:Rename", "menu:Import...") or the inline rename field ("rename")
    // was drawn in the last frame, in UI coordinates; nullopt when it wasn't. For tests.
    [[nodiscard]] std::optional<UiRect> controlRect(std::string_view label) const;

    // Where the path bar segment of `folder` ("" for Content) was drawn in the last frame, in UI
    // coordinates; nullopt when it wasn't. For tests.
    [[nodiscard]] std::optional<UiRect> segmentRect(std::string_view folder) const;

    // Where the item, or the tree row of a folder ("" for the root), was drawn in the last frame, in
    // UI coordinates; nullopt when it wasn't drawn.
    [[nodiscard]] std::optional<UiRect> itemRect(const ContentKey& item) const;
    [[nodiscard]] std::optional<UiRect> folderRect(std::string_view folder) const;

    // Where the drawer, the search box and the Filter button were drawn in the last frame; nullopt
    // when they weren't drawn.
    [[nodiscard]] std::optional<UiRect> drawerRect() const { return m_drawerRect; }

    [[nodiscard]] std::optional<UiRect> searchRect() const { return m_searchRect; }

    [[nodiscard]] std::optional<UiRect> filterRect() const { return m_filterRect; }

    // How many items the asset view drew in the last frame, and its grid's column count (1 in list
    // mode).
    [[nodiscard]] std::size_t itemsDrawn() const noexcept { return m_itemsDrawn; }

    [[nodiscard]] std::size_t gridColumns() const noexcept { return m_columns; }

    // Whether an unsaved-changes question waits for an answer.
    [[nodiscard]] bool asking() const noexcept { return m_pendingOpen.has_value(); }

private:
    // Which context menu a right-click asked for.
    enum class Menu : std::uint8_t {
        None,
        // The selected items.
        Items,
        // A tree folder (m_menuFolder).
        TreeFolder,
        // The asset view's empty space.
        EmptySpace,
    };

    // An import that waits for the frame after its notice (ADR-0049 rule 11).
    struct PendingImport {
        serialization::Value args;
        // ImGui's frame count when it was queued.
        int queuedFrame = 0;
    };

    // A drop target: a folder and where it was drawn.
    struct DropTarget {
        std::string folder;
        UiRect rect;
    };

    void drawBrowser();
    void drawToolbar();
    // The type filter's menu, while it is open; a change runs ith.content.set_filter.
    void drawFilterMenu(const ContentFilter& filter);
    void drawTree(float width, float height);
    void drawTreeRow(const ContentFolderRow& row);
    void drawPathBar();
    void drawItems();
    void drawGrid();
    // One grid tile of `width` x `height`: its background, icon, problem badge and label (or the
    // rename field), then its input.
    void drawTile(const ContentItem& item, float width, float height);
    void drawList();
    // Turns a click on a list header into ith.content.set_view, after the draw.
    void listSorting();
    void drawListRow(const ContentItem& item, bool searching);
    void drawStatus();
    void drawUnsavedQuestion();
    void drawContextMenu();
    void drawItemsMenu();
    void drawPromptDialogs();
    void drawDeleteDialog(const serialization::Value& details, std::vector<std::function<void()>>& after);
    void drawConflictDialog(const serialization::Value& details, std::vector<std::function<void()>>& after);
    void drawDropHighlight();
    void itemInput(const ContentItem& item);
    // The drag source of the item just drawn (`key`; `item` is null for a tree folder), and its drop
    // target when it is a folder.
    void dragAndDrop(const ContentKey& key, const ContentItem* item);
    // The drop target of the item just drawn, a folder at `folder`.
    void dropTarget(const std::string& folder);
    void keyboardInput();
    // Whether the item with `key` has the inline rename field.
    [[nodiscard]] bool renaming(const ContentKey& key) const;
    // The inline rename field, `width` wide with the extension after it.
    void drawRenameField(float width);
    // Records a labelled control just drawn, for controlRect().
    void recordControl(std::string_view label);
    // Why `m_dragged` can't move into `folder`; empty when it can.
    [[nodiscard]] std::string refusal(std::string_view folder) const;
    [[nodiscard]] std::optional<DropTarget> dropTargetAt(float x, float y) const;
    // Queues ith.content.import with `args` for the frame after the next notice.
    void queueImport(serialization::Value args);
    // Runs the queued import once its notice was drawn.
    void runPendingImport();
    // The status line after an open-file dialog couldn't be shown.
    void dialogUnavailable(std::string_view error);
    // Whether a question of the browser waits for its answer: a dialog shown in the last frame, the
    // unsaved-changes question, or the model's pending prompt.
    [[nodiscard]] bool questionOpen() const;
    // Refuses files that arrive while a question is open: a warning naming `what`, and the notice.
    void refuseWhileAsking(std::string_view what);
    // Sets the unsaved-changes question for `item` when it is a level and the open one is dirty.
    bool ask(const ContentKey& item);
    // Runs a command; a refusal is logged and becomes the notice, and a file operation's result
    // becomes the notice. Returns the result, or nullopt when it was refused.
    std::optional<serialization::Value> runCommand(std::string_view id, const serialization::Value& args);

    EditorSession* m_session;
    ContentBrowserModel* m_model;
    UiHost* m_host;
    bool m_focusSearch = false;
    // The focus item moved by a key: scroll it into view in the next draw.
    bool m_scrollToFocus = false;
    // Whether the panel had the keyboard focus this frame; drawDrawer() reports it.
    bool m_panelFocused = false;
    bool m_drawerWasOpen = false;
    // The tree's width while the splitter is dragged, as a share of the browser's width.
    std::optional<float> m_dragTree;
    // The drawer's height while its edge is dragged, in window coordinates.
    std::optional<float> m_dragHeight;
    // A level that waits for the unsaved-changes answer, and whether its question must open.
    std::optional<ContentKey> m_pendingOpen;
    bool m_openQuestion = false;
    // The drawer just opened: its asset view takes the keyboard focus.
    bool m_focusView = false;
    // What the rows asked for while drawing, run once the draw is done.
    std::vector<std::function<void()>> m_actions;
    std::size_t m_columns = 1;
    std::size_t m_itemsDrawn = 0;
    std::unordered_map<ContentKey, UiRect, ContentKeyHash> m_itemRects;
    std::unordered_map<std::string, UiRect> m_folderRects;
    std::optional<UiRect> m_drawerRect;
    std::optional<UiRect> m_searchRect;
    std::optional<UiRect> m_filterRect;
    // The panel window's rectangle when it was drawn this frame; drawDrawer() reports it.
    std::optional<ContentScreenRect> m_panelRect;
    // Where the browser (panel, drawer) was drawn in the last frame, for drops between frames.
    std::vector<UiRect> m_browserRects;
    // The path bar's segments of the last frame: each folder with its rectangle.
    std::vector<DropTarget> m_segments;
    // The labelled controls drawn this frame, and those of the last complete frame.
    std::unordered_map<std::string, UiRect> m_frameControls;
    std::unordered_map<std::string, UiRect> m_controls;
    std::string m_notice;
    // The context menu to open after the draw, and the tree folder it is for.
    Menu m_openMenu = Menu::None;
    Menu m_menu = Menu::None;
    std::string m_menuFolder;
    // A plain click on an item of a larger selection, which selects it alone on the release.
    std::optional<ContentKey> m_pressedSelected;
    // The items of the drag in progress between the browser's own widgets, why the target hovered in
    // the last frame refuses them, and whether a target took them this frame.
    std::vector<ContentKey> m_dragged;
    std::string m_dragRefusal;
    std::string m_frameRefusal;
    bool m_dragOverTarget = false;
    // The inline rename: the item, the field's text, the extension shown after it, whether the field
    // takes the keyboard focus in the next draw, and the error of a refused commit.
    std::optional<ContentKey> m_renameItem;
    // An NTFS name is up to 255 UTF-16 units, at most 3 UTF-8 bytes each (a surrogate pair is 2
    // units and 4 bytes); ext4 names are at most 255 bytes. The field holds any name on disk whole.
    static constexpr std::size_t kRenameTextSize = 1024;
    static_assert(kRenameTextSize > std::size_t{3} * 255, "the rename field must hold a whole NTFS name in UTF-8");
    std::array<char, kRenameTextSize> m_renameText{};
    std::string m_renameExtension;
    bool m_renameFocus = false;
    std::string m_renameError;
    // The dialog shown this frame and in the last one, and the conflict dialog's state: the file it
    // asks about, the answers so far ({file: choice}) and "Apply to all remaining".
    ContentDialog m_frameDialog = ContentDialog::None;
    ContentDialog m_dialog = ContentDialog::None;
    std::size_t m_conflict = 0;
    serialization::Value m_choices;
    bool m_applyToAll = false;
    // The open-file dialog: open, the folder it imports into, the files chosen so far, and the folder
    // of the last file chosen, where the next dialog starts.
    bool m_dialogOpen = false;
    std::string m_dialogTarget;
    std::vector<std::string> m_dialogFiles;
    std::string m_lastImportFolder;
    // An operating-system drag over the window: where it is, the files dropped so far and where the
    // first was dropped.
    std::optional<std::pair<float, float>> m_osDrag;
    // A DropPosition arrived since DropBegin: the drop has a real position.
    bool m_dragPositioned = false;
    std::vector<std::string> m_droppedFiles;
    std::pair<float, float> m_dropPoint{0.0f, 0.0f};
    std::optional<UiRect> m_dropHighlight;
    std::optional<PendingImport> m_pendingImport;
};

// Registers the "ith.content_browser" panel (Content Browser, docked at the bottom) drawing
// `panel`, and makes promptOpen() the prompt of ith.content.open. The panel removes the prompt when
// it is destroyed; the panel registry has no removal, so the session must not draw its panels after
// the panel is gone (the editor draws them only in its frame loop, which ends first).
void registerContentBrowser(EditorSession& session, ContentBrowserPanel& panel);

} // namespace ith
