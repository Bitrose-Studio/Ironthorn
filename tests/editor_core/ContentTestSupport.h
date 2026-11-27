#pragma once

// Helpers of the Content Browser tests (ContentBrowserTests.cpp, ContentFileOperationTests.cpp): fake
// importers, native asset text, and a temporary project with an asset database and a session over it.

#include "ith/asset_database/AssetDatabase.h"
#include "ith/asset_database/Importer.h"
#include "ith/editor_core/ContentBrowserModel.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/StringUtils.h"
#include "ith/platform_events/Event.h"

#include "EditorTestSupport.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ith::test::editor {

// The lines of `source` that start with `prefix`, without it.
inline std::vector<std::string> taggedLines(std::span<const std::byte> source, std::string_view prefix) {
    const std::string_view text(reinterpret_cast<const char*>(source.data()), source.size());
    std::vector<std::string> lines;
    for (const std::string_view line : split(text, '\n')) {
        if (line.starts_with(prefix)) {
            lines.emplace_back(line.substr(prefix.size()));
        }
    }
    return lines;
}

// Reads ".gltf" and ".glb" files: one mesh "Body", or one mesh per line "obj:<name>". Lines "dep:<path>"
// name the files it reads beside the source (Importer::dependencies), lines "ref:<path>" the files its
// objects refer to (ImportOutput::references), and a line "fail" fails the import. Enough for the
// browser, which never looks inside.
class FakeGltfImporter final : public ith::Importer {
public:
    [[nodiscard]] std::string_view id() const override { return "gltf"; }

    [[nodiscard]] std::uint32_t version() const override { return 1; }

    [[nodiscard]] std::span<const std::string_view> extensions() const override { return kExtensions; }

    [[nodiscard]] serialization::Value defaultSettings() const override { return serialization::Value::object(); }

    [[nodiscard]] ith::Expected<std::vector<std::string>>
    dependencies(std::span<const std::byte> source, const serialization::Value& /*settings*/) const override {
        return taggedLines(source, "dep:");
    }

    [[nodiscard]] ith::Expected<ith::ImportOutput> import(std::span<const std::byte> source,
                                                          std::span<const ith::ImportFile> /*files*/,
                                                          const serialization::Value& /*settings*/) const override {
        if (!taggedLines(source, "fail").empty()) {
            return makeError("import.malformed", "the test file says fail");
        }
        ith::ImportOutput output;
        for (std::string& name : taggedLines(source, "obj:")) {
            output.objects.push_back(ith::ImportedObject{.type = "mesh", .name = std::move(name), .payload = {}});
        }
        if (output.objects.empty()) {
            output.objects.push_back(ith::ImportedObject{.type = "mesh", .name = "Body", .payload = {}});
        }
        output.references = taggedLines(source, "ref:");
        return output;
    }

private:
    static constexpr std::array<std::string_view, 2> kExtensions = {".gltf", ".glb"};
};

// Reads ".png" files as one texture.
class FakePngImporter final : public ith::Importer {
public:
    [[nodiscard]] std::string_view id() const override { return "png"; }

    [[nodiscard]] std::uint32_t version() const override { return 1; }

    [[nodiscard]] std::span<const std::string_view> extensions() const override { return kExtensions; }

    [[nodiscard]] serialization::Value defaultSettings() const override { return serialization::Value::object(); }

    [[nodiscard]] ith::Expected<ith::ImportOutput> import(std::span<const std::byte> /*source*/,
                                                          std::span<const ith::ImportFile> /*files*/,
                                                          const serialization::Value& /*settings*/) const override {
        return ith::ImportOutput{.objects = {ith::ImportedObject{.type = "texture", .name = {}, .payload = {}}}};
    }

private:
    static constexpr std::array<std::string_view, 1> kExtensions = {".png"};
};

inline std::string nativeAsset(AssetId id, std::string_view format) {
    return R"({"format": ")" + std::string(format) + R"(", "version": 1, "id": ")" + id.toString() +
           R"(", "data": {}})";
}

// A project with an asset database (reading glTF and PNG files) and a session over it; no job system,
// so files are read and written at once.
struct Project {
    TempProject project;
    ith::ImporterRegistry importers;
    std::unique_ptr<ith::AssetDatabase> assets;
    std::unique_ptr<EditorSession> session;

    Project() {
        importers.add(std::make_unique<FakeGltfImporter>());
        importers.add(std::make_unique<FakePngImporter>());
        assets = std::make_unique<ith::AssetDatabase>(project.root(), importers);
        reopen();
    }

    // A new session over the same project, as the next run of the editor.
    void reopen() {
        session.reset();
        session = std::make_unique<EditorSession>(
            types(), ith::EditorSessionOptions{.projectDirectory = project.root(), .assets = assets.get()});
    }

    [[nodiscard]] std::string file(std::string_view path) const {
        return ith::fs::joinPath(project.root(), "Content/" + std::string(path));
    }

    void write(std::string_view path, std::string_view text) const {
        REQUIRE(ith::fs::writeFileAtomic(file(path), text).hasValue());
    }

    void folder(std::string_view path) const { REQUIRE(ith::fs::createDirectories(file(path)).hasValue()); }

    AssetId native(std::string_view path, std::string_view format = "ith.material",
                   AssetId id = AssetId::generate()) const {
        write(path, nativeAsset(id, format));
        return id;
    }

    AssetId imported(std::string_view path) const {
        write(path, "glTF");
        const auto report = assets->import(path);
        REQUIRE_MESSAGE(report.hasValue(), (report ? std::string() : report.error().message));
        return report->id;
    }

    serialization::Value execute(std::string_view id, std::string_view args = "{}") const {
        return run(*session, id, args);
    }

    void rescan() const { execute("ith.content.rescan"); }

    void navigate(std::string_view folder) const {
        execute("ith.content.navigate", R"({"folder": ")" + std::string(folder) + "\"}");
    }

    [[nodiscard]] ith::ContentBrowserModel& model() const { return session->content(); }

    // The labels of the shown items, in display order.
    [[nodiscard]] std::vector<std::string> labels() const {
        std::vector<std::string> result;
        for (const ith::ContentItem* item : model().shown()) {
            result.push_back(item->label);
        }
        return result;
    }

    [[nodiscard]] const ith::ContentItem& item(std::string_view path) const {
        const ith::ContentItem* found = model().findByPath(path);
        REQUIRE_MESSAGE(found != nullptr, std::string(path));
        return *found;
    }

    // The selection as content paths.
    [[nodiscard]] std::vector<std::string> selected() const {
        std::vector<std::string> paths;
        for (const ContentKey& key : model().selection()) {
            const ith::ContentItem* found = model().find(key);
            paths.push_back(found != nullptr ? found->path : std::string("?"));
        }
        return paths;
    }

    serialization::Value select(std::initializer_list<std::string_view> paths, std::string_view mode) const {
        std::string items = "[";
        for (const std::string_view path : paths) {
            items += (items.size() > 1 ? ", " : "") + std::string(R"({"path": ")") + std::string(path) + "\"}";
        }
        return execute("ith.content.select", R"({"items": )" + items + R"(], "mode": ")" + std::string(mode) + "\"}");
    }
};

inline const ith::ContentBadge* badgeOf(const ith::ContentItem& item, ith::AssetProblemKind kind) {
    for (const ith::ContentBadge& badge : item.problems) {
        if (badge.kind == kind) {
            return &badge;
        }
    }
    return nullptr;
}

inline bool exists(const std::string& path) {
    const auto info = ith::fs::fileInfo(path);
    return info && info->has_value();
}

inline ith::Event keyDown(ith::Key key, char32_t layoutKey = 0, ith::KeyModifiers modifiers = ith::KeyModifiers::None) {
    return ith::Event{.type = ith::EventType::KeyDown, .key = key, .layoutKey = layoutKey, .modifiers = modifiers};
}

} // namespace ith::test::editor
