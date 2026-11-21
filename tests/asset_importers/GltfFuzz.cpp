// libFuzzer target for the glTF importer (ADR-0015 rule 8: importers read files from anywhere and
// get fuzz targets). Built only in Clang sanitizer builds, which build the tools; CTest runs a short
// smoke run seeded with tests/asset_importers/fuzz_seeds/gltf, and longer runs are started by hand:
//
//   ith_gltf_import_fuzz -max_total_time=600 -dict=tests/asset_importers/fuzz_seeds/gltf.dict \
//       <corpus directory> tests/asset_importers/fuzz_seeds/gltf
//
// The input is the source file, ".gltf" or ".glb". Every external buffer it names gets the input's
// own bytes, so buffer views and accessors read fuzzed data either way.

#include "ith/asset_database/AssetDatabase.h"
#include "ith/asset_importers/GltfImporter.h"
#include "ith/asset_importers/ImportedModel.h"
#include "ith/serialization/BinaryArchive.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const ith::GltfImporter importer;
    const std::span<const std::byte> source(reinterpret_cast<const std::byte*>(data), size);
    const ith::serialization::Value settings = importer.defaultSettings();
    auto dependencies = importer.dependencies(source, settings);
    std::vector<ith::ImportFile> files;
    if (dependencies.hasValue()) {
        std::ranges::sort(*dependencies);
        dependencies->erase(std::ranges::unique(*dependencies).begin(), dependencies->end());
        for (const std::string& path : *dependencies) {
            // The importer only names paths that stay inside the source's folder.
            if (!ith::isValidContentPath(path)) {
                __builtin_trap();
            }
            files.push_back(ith::ImportFile{.path = path, .bytes = source});
        }
    }
    const auto output = importer.import(source, files, settings);
    if (!output.hasValue()) {
        if (!output.error().code.starts_with("import.")) {
            __builtin_trap();
        }
        return 0;
    }
    // Whatever imports has valid source keys, references and payloads that read back.
    if (!ith::sourceKeys(output->objects).hasValue()) {
        __builtin_trap();
    }
    for (const std::string& reference : output->references) {
        if (!ith::isValidContentPath(reference)) {
            __builtin_trap();
        }
    }
    for (const ith::ImportedObject& object : output->objects) {
        if (object.type == "mesh") {
            ith::ImportedMesh mesh;
            if (!ith::serialization::readBinary(object.payload, ith::kImportedMeshFormat, mesh).hasValue()) {
                __builtin_trap();
            }
            for (const ith::ImportedPrimitive& primitive : mesh.primitives) {
                const std::size_t vertices = primitive.positions.size() / 3;
                if (vertices == 0 || primitive.indices.size() % 3 != 0 ||
                    std::ranges::any_of(primitive.indices, [&](std::uint32_t index) { return index >= vertices; })) {
                    __builtin_trap();
                }
            }
        } else {
            ith::ImportedMaterial material;
            if (object.type != "material" ||
                !ith::serialization::readBinary(object.payload, ith::kImportedMaterialFormat, material).hasValue()) {
                __builtin_trap();
            }
        }
    }
    return 0;
}
