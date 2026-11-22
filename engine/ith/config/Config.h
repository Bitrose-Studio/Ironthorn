#pragma once

// Layered configuration (ADR-0035 rules 11 to 21, ADR-0008 rule 7).
//
// A Config reads settings from an ordered list of layer directories, lowest first (for the editor:
// engine, engine platform, project, project platform, user), and merges each file across them with
// JSON Merge Patch (RFC 7396). Settings are reflected structs whose field initializers are the
// engine defaults; a file holds only what differs:
//
//   struct EditorPreferences {
//       ITH_TYPE(Struct)
//       ITH_FIELD()
//       std::int32_t idleFramesPerSecond = 10;
//   };
//   constexpr ith::ConfigSection<EditorPreferences> kEditorPreferences{"Editor", "preferences"};
//
//   ith::Config config({{.name = "engine", .directory = engineDir},
//                       {.name = "user", .directory = userDir, .writable = true}});
//   EditorPreferences preferences = config.get(kEditorPreferences);
//   preferences.idleFramesPerSecond = 5;
//   ith::Expected<void> saved = config.set(kEditorPreferences, preferences); // into "user"
//
// A settings file is "<directory>/<file>.json", a document in the ADR-0010 envelope with format
// "ith.config", version 1, whose "data" is an object of named sections; comments are allowed, the
// files Config writes have none, and a file may be at most 1 MiB. A file that can't be read, fails
// to parse or has a duplicate key is skipped with one warning: that layer contributes nothing for
// the file. Merging, from the lowest layer to the highest: an object over an object merges key by
// key, recursively; any other value, arrays included, replaces the value below it; null removes
// the key. Lists that layers edit item by item are therefore objects keyed by a stable id.
//
// Documents that are not merged (layouts, input bindings) are whole files per layer: findFile,
// listFiles and writeFile.
//
// Change notification is by polling: generation() and generation(file) change on every set,
// writeFile and reload; consumers compare them once per frame and read again.
//
// Warnings (skipped files, unknown fields, values of the wrong type) are collected, each one once,
// in warnings(); the application logs them. A Config is not thread-safe: its owner on the main
// thread hands copies of sections to jobs.

#include "ith/foundation/Expected.h"
#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeOf.h"
#include "ith/serialization/Document.h"
#include "ith/serialization/Value.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ith {

// The envelope of a settings file.
inline constexpr serialization::DocumentFormat kConfigFormat{.name = "ith.config", .version = 1, .minimumVersion = 1};

// The largest settings file read, in bytes.
inline constexpr std::size_t kMaxConfigFileSize = std::size_t{1} << 20;

// One layer of a Config: a name ("engine", "project", "user", ...), its directory (a UTF-8 path)
// and whether Config may write into it.
struct ConfigLayer {
    std::string name;
    std::string directory;
    bool writable = false;
};

// Names a settings section: the reflected struct T in section `section` of "<file>.json". The
// strings must outlive the uses (string literals in practice).
template <typename T>
struct ConfigSection {
    std::string_view file;
    std::string_view section;
};

// A whole file found in a layer by findFile() or listFiles().
struct ConfigFile {
    // The file's name relative to the folder that was listed (listFiles), or the relative path
    // that was looked up (findFile).
    std::string name;
    // The index of the highest layer that has the file, into Config::layers().
    std::size_t layer = 0;
    // Its full UTF-8 path.
    std::string path;
};

// Applies the JSON Merge Patch `patch` to `target` (RFC 7396): when `patch` is an object, `target`
// becomes an object (if it isn't one) and each member of `patch` is merged into the member of the
// same key, null removing it; any other `patch` replaces `target`.
void applyMergePatch(serialization::Value& target, const serialization::Value& patch);

class Config {
public:
    // A Config over `layers`, lowest first. Files are read when they are first used. Preconditions
    // (asserted): at least one layer, unique names.
    explicit Config(std::vector<ConfigLayer> layers);

    [[nodiscard]] const std::vector<ConfigLayer>& layers() const noexcept { return m_layers; }

    // The index of the layer called `name`, or nullopt.
    [[nodiscard]] std::optional<std::size_t> findLayer(std::string_view name) const noexcept;

    // The merged data of "<file>.json" across every layer: an object of sections, empty when no
    // layer has the file. Valid until the next set or reload.
    [[nodiscard]] const serialization::Value& data(std::string_view file);

    // The merged section as T. Missing fields keep T's defaults. A field T doesn't have and a value
    // of the wrong type warn once ("config.unknown_field", "config.wrong_type"); the latter keeps
    // the field's default.
    template <typename T>
    [[nodiscard]] T get(const ConfigSection<T>& section) {
        T value{};
        read(section.file, section.section, typeOf<T>(), &value);
        return value;
    }

    // Writes `value` into the layer called `layer` (the highest layer when empty): the section of
    // that layer's file holds afterwards exactly the fields whose values differ from the merge of
    // the layers below it (defaults included), so a field that now equals them is removed and a
    // later change of a lower default still reaches it. Keys of the section that T doesn't have
    // are kept, existing keys keep their order and new ones are appended. The file is written at
    // once and durably. A std::optional field that is empty can't override a value below it (a
    // merge patch can't hold null) and is left out. Fails with the serialization errors of toValue
    // or writeDocument, or the file system's errors; on failure nothing changes. Preconditions
    // (asserted): the layer exists and is writable.
    template <typename T>
    [[nodiscard]] Expected<void> set(const ConfigSection<T>& section, const T& value, std::string_view layer = {}) {
        return write(section.file, section.section, typeOf<T>(), &value, layer);
    }

    // Type-erased get(): reads the section into `target`, a default-constructed value of `type`.
    void read(std::string_view file, std::string_view section, const TypeInfo& type, void* target);

    // Type-erased set().
    [[nodiscard]] Expected<void> write(std::string_view file, std::string_view section, const TypeInfo& type,
                                       const void* value, std::string_view layer = {});

    // The highest layer that has the file `relative` (e.g. "Layouts/Default.json"), or nullopt.
    // Fails with "config.invalid_path" for an absolute path or one that leaves the layer.
    [[nodiscard]] Expected<std::optional<ConfigFile>> findFile(std::string_view relative);

    // The files in `folder` (relative, "" for the layer directories themselves) across every layer,
    // by name in byte order, each from the highest layer that has it. Missing folders contribute
    // nothing; temporary files of interrupted writes are left out. Fails with
    // "config.invalid_path" as findFile.
    [[nodiscard]] Expected<std::vector<ConfigFile>> listFiles(std::string_view folder);

    // Writes the whole file `relative` into the layer called `layer`, durably. Fails with
    // "config.invalid_path" or the file system's errors. Preconditions (asserted): the layer exists
    // and is writable.
    [[nodiscard]] Expected<void> writeFile(std::string_view layer, std::string_view relative, std::string_view bytes);

    // Changes on every set, writeFile and reload.
    [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }

    // Changes on every set of the settings file `file` ("Editor"), every writeFile of the whole
    // file `file` ("Layouts/Default.json") and every reload.
    [[nodiscard]] std::uint64_t generation(std::string_view file) const;

    // Reads every file that was read before again, for a Reload command until a file watcher exists.
    void reload();

    // The warnings so far, each one once (until reload).
    [[nodiscard]] const std::vector<Error>& warnings() const noexcept { return m_warnings; }

    // Returns the warnings so far and forgets them; the same warning doesn't come back until reload.
    [[nodiscard]] std::vector<Error> takeWarnings();

private:
    struct LoadedFile {
        // The data of the file in each layer; nullopt where the layer has none or it was skipped.
        std::vector<std::optional<serialization::Value>> layers;
        serialization::Value merged;
    };

    LoadedFile& load(std::string_view file);
    void readLayers(std::string_view file, LoadedFile& loaded);
    [[nodiscard]] static serialization::Value mergeBelow(const LoadedFile& loaded, std::size_t layer);
    void readSection(const serialization::Value* section, std::string_view file, std::string_view sectionName,
                     const TypeInfo& type, void* target, bool report);
    void warn(std::string code, std::string message);
    [[nodiscard]] std::size_t writableLayer(std::string_view name) const;
    void bump(std::string_view file);

    std::vector<ConfigLayer> m_layers;
    std::map<std::string, LoadedFile, std::less<>> m_files;
    std::map<std::string, std::uint64_t, std::less<>> m_fileGenerations;
    std::uint64_t m_generation = 0;
    std::uint64_t m_reloads = 0;
    std::vector<Error> m_warnings;
    std::set<std::string, std::less<>> m_warned;
};

} // namespace ith
