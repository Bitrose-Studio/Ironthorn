#pragma once

// ith::ContentBrowserPanel: the Content Browser panel and the content drawer (ADR-0008 rules 2 and
// 4, ADR-0049 rules 4, 7 and 26, docs/specs/content-browser.md), drawn from editor_core's
// ContentBrowserModel; both show the same model.
//
//   ith::ContentBrowserPanel content(session);
//   ith::registerContentBrowser(session, content); // the panel, and the prompt of ith.content.open
//   // every frame, inside the UI frame:
//   ith::drawEditorShell(session, keymap, shell, dock);  // draws the panel when it is open
//   content.drawDrawer();                                // the drawer, and the focus report
//
// The browser: a toolbar (the search box, the type filter menu, grid or list, the sort menu,
// Rescan), the folder tree beside the path bar and the asset view, with a splitter between them, and
// a status line ("<n> items", "<m> selected", "<p> problems"; a click on the problem count shows the
// Problems filter). The tree and the asset view draw only their visible rows (list clippers); the
// list sorts by a click on its Name or Type header. A click selects (Ctrl toggles, Shift selects the
// range from the anchor, Ctrl + Shift adds it, a click on empty space clears); a double-click opens:
// a folder becomes current, a level opens. The arrow keys, Home and End move the focus item and
// select (Shift: the range), Ctrl + F goes to the search box. Every change runs an ith.content
// command; a refused one is logged in the "ContentBrowser" category.
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

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ith {

class EditorSession;

class ContentBrowserPanel {
public:
    // `session` must outlive the panel.
    explicit ContentBrowserPanel(EditorSession& session);

    ContentBrowserPanel(const ContentBrowserPanel&) = delete;
    ContentBrowserPanel& operator=(const ContentBrowserPanel&) = delete;
    ContentBrowserPanel(ContentBrowserPanel&&) = delete;
    ContentBrowserPanel& operator=(ContentBrowserPanel&&) = delete;
    // Removes the prompt of ith.content.open that registerContentBrowser set.
    ~ContentBrowserPanel();

    // Draws the panel's content inside its window. Main thread, inside a UI frame.
    void draw();

    // Draws the drawer when it is open and the unsaved-changes question when one is pending, then
    // reports the browser's keyboard focus of this frame (panel or drawer) to the model. Call once per
    // frame, after the panels, also while the drawer is closed. Main thread, inside a UI frame.
    void drawDrawer();

    // Opens `item` as a double-click does: asks first when the open level has unsaved changes and
    // the item is a level; otherwise runs ith.content.open. True when it asked.
    bool open(const ContentKey& item);

    // The prompt of ith.content.open (Enter): open() on the primary item; false when nothing is
    // selected or nothing needs asking.
    bool promptOpen();

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
    void drawBrowser();
    void drawToolbar();
    void drawTree(float width, float height);
    void drawPathBar();
    void drawItems();
    void drawGrid();
    // One grid tile of `width` x `height`: its background, icon, problem badge and label, then its
    // input.
    void drawTile(const ContentItem& item, float width, float height);
    void drawList();
    // Turns a click on a list header into ith.content.set_view, after the draw.
    void listSorting();
    void drawListRow(const ContentItem& item, bool searching);
    void drawStatus();
    void drawUnsavedQuestion();
    void itemInput(const ContentItem& item);
    void keyboardInput();
    // Sets the unsaved-changes question for `item` when it is a level and the open one is dirty.
    bool ask(const ContentKey& item);
    void runCommand(std::string_view id, const serialization::Value& args);

    EditorSession* m_session;
    ContentBrowserModel* m_model;
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
};

// Registers the "ith.content_browser" panel (Content Browser, docked at the bottom) drawing
// `panel`, and makes promptOpen() the prompt of ith.content.open. The panel removes the prompt when
// it is destroyed; the panel registry has no removal, so the session must not draw its panels after
// the panel is gone (the editor draws them only in its frame loop, which ends first).
void registerContentBrowser(EditorSession& session, ContentBrowserPanel& panel);

} // namespace ith
