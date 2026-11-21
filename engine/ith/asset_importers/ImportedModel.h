#pragma once

// The payloads of imported meshes and materials: what a model importer (GltfImporter.h) puts in
// ImportedObject::payload.
//
// Each payload is a binary archive blob (ith/serialization/BinaryArchive.h) of one of the types
// below, with its own format name and version, so the cooker can read it back with readBinary:
//
//   ith::ImportedMesh mesh;
//   const auto read = ith::serialization::readBinary(payload, ith::kImportedMeshFormat, mesh);
//
// This is the importer's output, not the runtime mesh format: cooking turns it into GPU-ready
// data later. Increase a format's version, and the importers' versions, whenever a type changes.

#include "ith/reflection/Annotations.h"
#include "ith/serialization/Document.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ith {

// One triangle list of a mesh, with one material.
struct ImportedPrimitive {
    ITH_TYPE(Struct)
    // x, y, z per vertex: the vertex count is positions.size() / 3, at least 1.
    ITH_FIELD()
    std::vector<float> positions;
    // x, y, z per vertex, as stored in the source (not renormalized); empty when it has none.
    ITH_FIELD()
    std::vector<float> normals;
    // u, v per vertex of the first texture coordinate set; empty when it has none.
    ITH_FIELD()
    std::vector<float> texCoords;
    // Three vertex indices per triangle, each less than the vertex count. A source without
    // indices gets 0, 1, 2, ... so that every primitive is indexed.
    ITH_FIELD()
    std::vector<std::uint32_t> indices;
    // The source key of the primitive's material among the same file's sub-assets, such as
    // "material:Steel" (sourceKeys in ith/asset_database/Importer.h); empty for the default
    // material.
    ITH_FIELD()
    std::string material;
};

// A mesh: its primitives, in source order.
struct ImportedMesh {
    ITH_TYPE(Struct)
    ITH_FIELD()
    std::vector<ImportedPrimitive> primitives;
};

// How a material's alpha is used.
enum class ImportedAlphaMode : std::uint8_t {
    Opaque, // alpha is ignored
    Mask,   // a pixel is drawn when its alpha is at least alphaCutoff
    Blend,  // alpha blends with what is behind
};
ITH_ENUM(ImportedAlphaMode)

// A texture a material uses.
struct ImportedTexture {
    ITH_TYPE(Struct)
    // The image file, relative to the source file's folder (also listed in
    // ImportOutput::references); empty when the image is embedded in the source, which isn't
    // imported yet.
    ITH_FIELD()
    std::string image;
    // The texture coordinate set it uses: 0 for ImportedPrimitive::texCoords.
    ITH_FIELD()
    std::uint32_t texCoord = 0;
};

// A metallic-roughness material. Factors multiply the matching texture where there is one.
struct ImportedMaterial {
    ITH_TYPE(Struct)
    // Linear red, green, blue and alpha.
    ITH_FIELD()
    std::array<float, 4> baseColorFactor = {1.0F, 1.0F, 1.0F, 1.0F};
    ITH_FIELD()
    float metallicFactor = 1.0F;
    ITH_FIELD()
    float roughnessFactor = 1.0F;
    // Linear red, green and blue.
    ITH_FIELD()
    std::array<float, 3> emissiveFactor = {0.0F, 0.0F, 0.0F};
    ITH_FIELD()
    ImportedAlphaMode alphaMode = ImportedAlphaMode::Opaque;
    ITH_FIELD()
    float alphaCutoff = 0.5F;
    ITH_FIELD()
    bool doubleSided = false;
    // Linear base color in RGB, alpha in A.
    ITH_FIELD()
    std::optional<ImportedTexture> baseColorTexture;
    // Roughness in G, metalness in B.
    ITH_FIELD()
    std::optional<ImportedTexture> metallicRoughnessTexture;
    // A tangent-space normal map, its X and Y scaled by normalScale.
    ITH_FIELD()
    std::optional<ImportedTexture> normalTexture;
    ITH_FIELD()
    float normalScale = 1.0F;
    // Ambient occlusion in R, applied with occlusionStrength (0 to 1).
    ITH_FIELD()
    std::optional<ImportedTexture> occlusionTexture;
    ITH_FIELD()
    float occlusionStrength = 1.0F;
    // Emitted color in RGB.
    ITH_FIELD()
    std::optional<ImportedTexture> emissiveTexture;
};

// The payload formats of sub-assets of type "mesh" and "material".
inline constexpr serialization::DocumentFormat kImportedMeshFormat{.name = "ith.imported_mesh", .version = 1};
inline constexpr serialization::DocumentFormat kImportedMaterialFormat{.name = "ith.imported_material", .version = 1};

} // namespace ith
