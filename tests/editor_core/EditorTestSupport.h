#pragma once

// Helpers of the editor_core tests: a type registry with the ecs, editor_core, shapes and test types,
// commands run with JSON text arguments, and the world as level text, which is the same for the
// same world (ADR-0034 rule 9).

#include "ith/config/Config.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/PanelRegistry.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/Uuid.h"
#include "ith/level/Level.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/serialization/Json.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream> // doctest prints std::string operands through an ostream
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

ITH_DECLARE_REFLECTION_MODULE(ecs)
ITH_DECLARE_REFLECTION_MODULE(editor_core)
ITH_DECLARE_REFLECTION_MODULE(shapes)
ITH_DECLARE_REFLECTION_MODULE(test_editor_core)

namespace ith::test::editor {

inline const TypeRegistry& types() {
    static const TypeRegistry kTypes = [] {
        TypeRegistry result;
        generated::registerReflection_ecs(result);
        generated::registerReflection_editor_core(result);
        generated::registerReflection_shapes(result);
        generated::registerReflection_test_editor_core(result);
        return result;
    }();
    return kTypes;
}

// A stable id for tests: n in the low bits.
inline Uuid idOf(std::uint64_t n) {
    return Uuid(0xED, n);
}

inline std::string idText(std::uint64_t n) {
    return idOf(n).toString();
}

inline serialization::Value json(std::string_view text) {
    auto parsed = serialization::parseJson(text);
    REQUIRE_MESSAGE(parsed.hasValue(), (parsed.hasValue() ? std::string() : parsed.error().message));
    return std::move(*parsed);
}

// Runs a command that must succeed.
inline serialization::Value run(EditorSession& session, std::string_view id, std::string_view args = "{}") {
    auto result = session.execute(id, json(args));
    REQUIRE_MESSAGE(result.hasValue(),
                    (std::string(id) + ": " + (result.hasValue() ? std::string() : result.error().message)));
    return std::move(*result);
}

// Runs a command that must fail with `code`.
inline void fails(EditorSession& session, std::string_view id, std::string_view args, std::string_view code) {
    auto result = session.execute(id, json(args));
    REQUIRE_FALSE(result.hasValue());
    CAPTURE(result.error().message);
    CHECK(result.error().code == code);
}

// The world as level text.
inline std::string levelText(const EditorSession& session) {
    auto text = saveLevel(session.document().world(), session.document().levelId(), types());
    REQUIRE_MESSAGE(text.hasValue(), (text.hasValue() ? std::string() : text.error().message));
    return std::move(*text);
}

inline void removeTree(const std::string& path) {
    const Expected<std::optional<fs::FileInfo>> info = fs::fileInfo(path);
    if (!info || !info->has_value()) {
        return;
    }
    if ((*info)->kind == fs::FileKind::Directory) {
        if (Expected<std::vector<fs::DirectoryEntry>> entries = fs::listDirectory(path)) {
            for (const fs::DirectoryEntry& entry : *entries) {
                removeTree(fs::joinPath(path, entry.name));
            }
        }
    }
    static_cast<void>(fs::remove(path));
}

// The configuration of a test editor (ADR-0035 rule 12): the shipped engine layer (the tests run in
// the repository root), the project's Config folder and a user layer inside the project, so tests
// never touch a developer's own user layer.
inline Config projectConfig(const std::string& projectRoot) {
    return Config({{.name = "engine", .directory = "engine/config"},
                   {.name = "project", .directory = fs::joinPath(projectRoot, "Config")},
                   {.name = "user", .directory = fs::joinPath(projectRoot, "User"), .writable = true}});
}

// The panels the editor registers today, as editor_ui does but without views (ADR-0040 rule 3).
inline void addEditorPanels(PanelRegistry& panels) {
    panels.add({.id = "ith.outliner", .displayName = "Outliner", .defaultDock = DockPlace::Right});
    panels.add({.id = "ith.details", .displayName = "Details", .defaultDock = DockPlace::Right});
    panels.add({.id = "ith.output_log", .displayName = "Output Log", .defaultDock = DockPlace::Bottom});
}

// Every P2 panel of ADR-0040 rule 3, without views.
inline void addP2Panels(PanelRegistry& panels) {
    panels.add({.id = "ith.viewport", .displayName = "Viewport", .defaultDock = DockPlace::Center, .maxInstances = 4});
    panels.add({.id = "ith.outliner", .displayName = "Outliner", .defaultDock = DockPlace::Right});
    panels.add({.id = "ith.details", .displayName = "Details", .defaultDock = DockPlace::Right});
    panels.add({.id = "ith.content_browser", .displayName = "Content Browser", .defaultDock = DockPlace::Bottom});
    panels.add({.id = "ith.output_log", .displayName = "Output Log", .defaultDock = DockPlace::Bottom});
}

// An empty project directory in the temporary folder, removed afterwards.
class TempProject {
public:
    TempProject() {
        std::error_code error;
        const std::u8string base = std::filesystem::temp_directory_path(error).generic_u8string();
        REQUIRE_FALSE(error);
        m_root = fs::joinPath(std::string(base.begin(), base.end()), "ith-editor-test-" + Uuid::generate().toString());
        REQUIRE(fs::createDirectories(fs::joinPath(m_root, "Content")).hasValue());
    }

    TempProject(const TempProject&) = delete;
    TempProject& operator=(const TempProject&) = delete;
    TempProject(TempProject&&) = delete;
    TempProject& operator=(TempProject&&) = delete;

    ~TempProject() { removeTree(m_root); }

    [[nodiscard]] const std::string& root() const noexcept { return m_root; }

private:
    std::string m_root;
};

} // namespace ith::test::editor
