#pragma once

// The glTF 2.0 importer (ADR-0015 rule 8): meshes and materials from ".gltf" and ".glb" files.
//
//   ith::ImporterRegistry importers;
//   importers.add(std::make_unique<ith::GltfImporter>());
//
// What it reads:
// - ".gltf" (JSON) and ".glb" (binary container: a JSON chunk and an optional BIN chunk) files;
//   buffers in the BIN chunk, in base64 "data:" URIs, or in files next to the source, named by
//   relative URIs that stay inside the source file's folder (no "..", absolute paths, drive
//   letters or other schemes; percent escapes are decoded first). Those files are the importer's
//   dependencies.
// - Every mesh, as a sub-asset "mesh" whose payload is an ImportedMesh (ImportedModel.h): per
//   triangle primitive its POSITION, NORMAL, TEXCOORD_0, indices and material. Other attributes
//   and morph targets are ignored. Other primitive modes (points, lines, strips, fans) fail the
//   import with "import.unsupported".
// - Every material, as a sub-asset "material" whose payload is an ImportedMaterial: the
//   metallic-roughness factors, alpha mode and cutoff, double-sidedness and texture slots. The
//   image files of its textures are listed in ImportOutput::references; embedded images are not
//   imported yet.
// - Accessors in full: every component type, normalized integers, byte strides, sparse
//   accessors and accessors without a buffer view (zeros). min and max are ignored.
//
// Names (ADR-0015 rule 3): a mesh is named by its own name, or, when it has none, by the name
// path of the first node in node order that uses it (the names of its named ancestors and its
// own, joined by '/'); a material by its name. Unnamed objects fall back to their index among
// the unnamed objects of their type.
//
// The file is untrusted. Every index, offset, length and stride is checked with overflow-safe
// arithmetic, every index of a primitive is checked against its vertex count, and floats must be
// finite. What the import builds beyond the parsed JSON (decoded buffers, accessor data,
// output arrays, and every copy of a name, path or key) counts against a budget: the larger of
// kGltfMinDecodeBudget and kGltfDecodeBudgetFactor times the size of the source and its buffers,
// at most kGltfMaxDecodeBudget. Names, node paths and file URIs are at most kGltfMaxNameSize
// bytes. Files that require extensions (extensionsRequired) fail with "import.unsupported".
//
// dependencies() and import() each parse the JSON: the asset database calls dependencies() for
// every import to build the cache key, and import() only on a cache miss.
//
// Errors: "import.malformed" (the file breaks the glTF 2.0 specification; the message names the
// JSON path, such as "meshes[0].primitives[1].indices"), "import.unsupported", "import.invalid_uri"
// and "import.too_large".
//
// Thread-safe and deterministic, like every importer. It has no settings.

#include "ith/asset_database/Importer.h"
#include "ith/foundation/Expected.h"
#include "ith/serialization/Value.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ith {

// The least that decoded data may take, in bytes.
inline constexpr std::uint64_t kGltfMinDecodeBudget = std::uint64_t{64} << 20;

// How many times the size of the source and its buffers decoded data may take.
inline constexpr std::uint64_t kGltfDecodeBudgetFactor = 16;

// The most that decoded data may take, whatever the size of the input, in bytes.
inline constexpr std::uint64_t kGltfMaxDecodeBudget = std::uint64_t{4} << 30;

// The longest name (of a mesh, material or node), node path or file URI, in bytes.
inline constexpr std::size_t kGltfMaxNameSize = 4096;

class GltfImporter final : public Importer {
public:
    // "gltf".
    [[nodiscard]] std::string_view id() const override;

    [[nodiscard]] std::uint32_t version() const override;

    // ".gltf" and ".glb".
    [[nodiscard]] std::span<const std::string_view> extensions() const override;

    // An empty object: the importer has no settings yet.
    [[nodiscard]] serialization::Value defaultSettings() const override;

    // The external buffer files of the source, decoded from their URIs. Fails like import().
    [[nodiscard]] Expected<std::vector<std::string>> dependencies(std::span<const std::byte> source,
                                                                  const serialization::Value& settings) const override;

    // The meshes, then the materials, of the source. ".glb" is told from ".gltf" by its magic
    // number, not by the file name.
    [[nodiscard]] Expected<ImportOutput> import(std::span<const std::byte> source, std::span<const ImportFile> files,
                                                const serialization::Value& settings) const override;

private:
    static constexpr std::array<std::string_view, 2> kExtensions = {".gltf", ".glb"};
};

} // namespace ith
