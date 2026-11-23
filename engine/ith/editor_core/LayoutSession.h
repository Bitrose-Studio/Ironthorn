#pragma once

// ith::LayoutSession: the editor's current layout, its named layouts and its session state
// (ADR-0040 rules 10 to 17), without any UI.
//
//   session.layouts().restore();               // at start-up, once the panels are registered
//   session.execute("ith.window.open_panel", args);
//   if (layouts.generation() != applied) { /* the UI rebuilds its docking from current() */ }
//   layouts.observe(readBack);                 // the UI reports what the user arranged
//
// Named layouts are ith.editor_layout documents, "Layouts/<name>.json" in the layered
// configuration (ADR-0035 rule 18): the engine ships "Default", a project may add its own, and Save
// Layout As writes the user layer, which is the configuration's highest layer. The chosen default is
// the "layouts" section of Editor.json. The last session's layout is written to the session file
// (<project>/Saved/Editor/Layout.json) whenever the layout differs from what the file holds.
//
// Layout changes are not world edits: they make no undo entry and don't mark the level dirty.
// Warnings (dropped panels, files that fail to load) are logged in the "EditorLayout" category and
// returned by the calls that load layouts. Main thread only.

#include "ith/config/Config.h"
#include "ith/editor_core/EditorLayout.h"
#include "ith/editor_core/PanelRegistry.h"
#include "ith/foundation/Expected.h"
#include "ith/reflection/Annotations.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ith {

// The "layouts" section of Editor.json (ADR-0040 rule 15).
struct EditorLayoutSettings {
    ITH_TYPE(Struct)
    // The layout Reset Layout loads. The field initializer is the engine's default.
    ITH_FIELD()
    std::string defaultName = "Default";
};

inline constexpr ConfigSection<EditorLayoutSettings> kEditorLayoutSettings{"Editor", "layouts"};

// The folder of named layouts in every configuration layer.
inline constexpr std::string_view kLayoutFolder = "Layouts";

// A named layout and the configuration layer it comes from (the highest that has it).
struct LayoutListing {
    std::string name;
    std::string layer;
    // In the user layer: Save Layout As wrote it, and Delete Layout can remove it.
    bool user = false;
};

// What loading a layout gave: its name (empty for the session file and the built-in default) and
// the warnings of its validation.
struct LayoutLoad {
    std::string name;
    std::vector<Error> warnings;
};

// What Open Panel did: the instance it names, and whether it opened it (false: it was open and is
// focused instead).
struct PanelOpened {
    std::string instance;
    bool opened = false;
};

class LayoutSession {
public:
    // `config`, when not null, holds the named layouts and the default choice; without it only the
    // built-in default exists and the commands that touch files fail with "editor.layout.no_config".
    // `sessionFile` is the session state's path; empty: none is read or written. `panels` and
    // `config` must outlive the session. The current layout starts as one empty central stack.
    LayoutSession(const PanelRegistry& panels, Config* config, std::string sessionFile);

    LayoutSession(const LayoutSession&) = delete;
    LayoutSession& operator=(const LayoutSession&) = delete;
    LayoutSession(LayoutSession&&) = delete;
    LayoutSession& operator=(LayoutSession&&) = delete;
    ~LayoutSession() = default;

    [[nodiscard]] const EditorLayout& current() const noexcept { return m_current; }

    // The name of the layout last loaded, reset to or saved; empty after the session file.
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }

    // Changes when a command replaces the layout, so the UI rebuilds its docking from current().
    [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }

    // Changes on every change of the layout, observe() included, so the UI draws a frame.
    [[nodiscard]] std::uint64_t changes() const noexcept { return m_changes; }

    // Called before a command reads the current layout, so the UI can observe() what is on screen
    // first. Null: nothing is called.
    void setReadBack(std::function<void()> readBack) { m_readBack = std::move(readBack); }

    // Stores what the user arranged, normalized without warnings, as the current layout, without
    // changing generation(); writes the session file when it differs from it.
    void observe(EditorLayout layout);

    // Start-up: the session file's layout, or reset() when it is missing or fails to load.
    LayoutLoad restore();

    // Loads the named layout (matched case-insensitively). Fails with "editor.layout.not_found",
    // "editor.layout.invalid" and "editor.layout.no_config".
    Expected<LayoutLoad> load(std::string_view name);

    // Saves the current layout as a user layout. Fails with "editor.layout.invalid_name",
    // "editor.layout.reserved_name" (an engine or project layout has the name),
    // "editor.layout.exists" (a user layout has it and `overwrite` is false), "editor.layout.no_config"
    // and the file errors.
    Expected<std::string> saveAs(std::string_view name, bool overwrite);

    // Makes the named layout the user's default; it must load. Fails as load() does, and with the
    // configuration's write errors. Returns the layout's name as the files spell it.
    Expected<std::string> setDefault(std::string_view name);

    // Deletes a user layout; deleting the user's default also removes that choice. Fails with
    // "editor.layout.not_found", "editor.layout.reserved_name" (an engine or project layout),
    // "editor.layout.no_config" and the file errors.
    Expected<std::string> remove(std::string_view name);

    // Loads the default (ADR-0040 rule 16): the chosen default, the default below the user layer,
    // "Default", then the built-in default. Always ends with a layout.
    LayoutLoad reset();

    // Opens a panel instance, or the lowest closed instance of a multi-instance panel's id, at its
    // default place; an open one is focused. Fails with "editor.layout.unknown_panel" and
    // "editor.layout.no_free_instance".
    Expected<PanelOpened> open(std::string_view panel);

    // Closes an instance; false when it wasn't open. Fails with "editor.layout.unknown_panel".
    Expected<bool> close(std::string_view instance);

    // The instance Open Panel asked to focus since the last call, if any.
    [[nodiscard]] std::optional<std::string> takeFocusRequest();

    // The named layouts, by name, each from the highest layer that has it.
    [[nodiscard]] Expected<std::vector<LayoutListing>> list();

    // The chosen default's name (merged across the layers).
    [[nodiscard]] std::string defaultName();

    // Writes the session file when the current layout differs from what it holds. A failure is
    // logged once until a write succeeds.
    void saveSession();

private:
    struct Found {
        std::string name;
        std::size_t layer = 0;
        std::string path;
    };

    Expected<std::optional<Found>> find(std::string_view name);
    Expected<EditorLayout> loadFile(const Found& found, std::vector<Error>& warnings);
    [[nodiscard]] Expected<void> needConfig() const;
    [[nodiscard]] Expected<std::size_t> userLayer() const;
    void replace(EditorLayout layout, std::string name);
    void readBack();

    const PanelRegistry* m_panels;
    Config* m_config;
    std::string m_sessionFile;
    EditorLayout m_current;
    std::string m_name;
    std::uint64_t m_generation = 0;
    std::uint64_t m_changes = 0;
    std::function<void()> m_readBack;
    std::optional<std::string> m_focusRequest;
    // What the session file holds, once read or written.
    std::optional<EditorLayout> m_written;
    bool m_sessionWriteFailed = false;
};

} // namespace ith
