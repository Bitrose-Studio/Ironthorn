#pragma once

// Importers turn source files (glTF, PNG, WAV, ...) into engine data (ADR-0015 rules 8 and 9).
//
// An importer reads the bytes of one source file with its settings and returns the objects in it:
// meshes, materials, skeletons, animations. Each object has a type and, where the source names
// it, a name path; the asset database derives each object's stable source key from these and
// keeps the sub-asset ids in the file's sidecar, so a re-exported file keeps its ids even when its
// objects are reordered.
//
//   class TextureImporter final : public ith::Importer {
//   public:
//       std::string_view id() const override { return "png"; }
//       std::uint32_t version() const override { return 1; }
//       std::span<const std::string_view> extensions() const override { return kExtensions; }
//       ith::serialization::Value defaultSettings() const override { ... }
//       ith::Expected<ith::ImportOutput> import(std::span<const std::byte> source,
//                                               const ith::serialization::Value& settings) const override;
//   };
//
//   ith::ImporterRegistry importers;
//   importers.add(std::make_unique<TextureImporter>());
//
// Importers are tools-only. Each one gets a fuzz target, because source files come from anywhere.

#include "ith/foundation/Expected.h"
#include "ith/serialization/Value.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ith {

// One object an importer found in a source file.
struct ImportedObject {
    // What it is: lowercase ASCII letters, digits and '_', e.g. "mesh" or "animation".
    std::string type;
    // Its name path in the source, '/'-separated from the outermost named parent, e.g.
    // "Body/Hull"; empty when the object has no name. Any UTF-8 text.
    std::string name;
    // The imported data, in the importer's output format.
    std::vector<std::byte> payload;

    friend bool operator==(const ImportedObject&, const ImportedObject&) = default;
};

// What an importer returns: the objects of the source file, in source order.
struct ImportOutput {
    std::vector<ImportedObject> objects;
};

// Reads one kind of source file.
//
// Implementations must be deterministic (the same bytes and settings give the same output, which
// the derived-data cache relies on), must treat the source as untrusted, and must be thread-safe:
// import() may run on several files at once.
class Importer {
public:
    Importer() = default;
    Importer(const Importer&) = delete;
    Importer& operator=(const Importer&) = delete;
    Importer(Importer&&) = delete;
    Importer& operator=(Importer&&) = delete;
    virtual ~Importer() = default;

    // A stable identifier stored in sidecars, e.g. "gltf". Never changes once files use it.
    [[nodiscard]] virtual std::string_view id() const = 0;

    // The version of the importer's output. Increase it whenever the output for the same input
    // changes, so the derived-data cache imports again.
    [[nodiscard]] virtual std::uint32_t version() const = 0;

    // The file extensions it reads, lowercase with the dot, e.g. ".gltf" and ".glb".
    [[nodiscard]] virtual std::span<const std::string_view> extensions() const = 0;

    // The settings of a newly imported file: a JSON object.
    [[nodiscard]] virtual serialization::Value defaultSettings() const = 0;

    // Imports `source` with `settings` (an object; keys the importer doesn't know must be ignored,
    // so that older settings still import). Fails with an error whose code starts with "import."
    // when the source is malformed.
    [[nodiscard]] virtual Expected<ImportOutput> import(std::span<const std::byte> source,
                                                        const serialization::Value& settings) const = 0;
};

// The importers the asset database can use. Filled at startup, then only read; reads are
// thread-safe, add() is not.
class ImporterRegistry {
public:
    // Adds an importer. Preconditions (asserted): `importer` is set, its id and extensions are not
    // empty, extensions are lowercase and start with '.', and neither its id nor any of its
    // extensions is already registered.
    void add(std::unique_ptr<Importer> importer);

    // The importer with this id, or null.
    [[nodiscard]] const Importer* find(std::string_view id) const noexcept;

    // The importer for the extension of `path` (UTF-8, '/'-separated), compared without ASCII
    // case, or null.
    [[nodiscard]] const Importer* findForPath(std::string_view path) const;

private:
    // The importer of a lowercase extension such as ".png", or null.
    [[nodiscard]] const Importer* findForExtension(std::string_view extension) const noexcept;

    std::vector<std::unique_ptr<Importer>> m_importers;
};

// The stable source keys of `objects`, in the same order (ADR-0015 rule 3):
// - a named object: "<type>:<name>", where '%' and '#' in the name are written "%25" and "%23";
// - a further object of the same type with the same name: "<type>:<name>#<n>", n = 1, 2, ...;
// - an unnamed object: "<type>:#<n>", its index among the unnamed objects of its type.
// Fails with "import.invalid_output" when a type is empty or has other characters than lowercase
// ASCII letters, digits and '_'.
[[nodiscard]] Expected<std::vector<std::string>> sourceKeys(std::span<const ImportedObject> objects);

} // namespace ith
