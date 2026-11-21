// The glTF 2.0 importer (ADR-0015 rule 8): embedded, external and GLB buffers, accessors, meshes,
// materials, source keys, reimport through the asset database, the derived-data cache, and
// malformed input.

#include "ith/asset_importers/GltfImporter.h"

#include "ith/asset_database/AssetDatabase.h"
#include "ith/asset_importers/ImportedModel.h"
#include "ith/serialization/BinaryArchive.h"
#include "ith/serialization/Json.h"

#include "../asset_database/AssetTestSupport.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace serialization = ith::serialization;
using serialization::Value;

static_assert(std::endian::native == std::endian::little, "the test buffers are written in the host's byte order");

// One triangle: positions, normals and texture coordinates (floats) in buffer view 0, three
// unsigned short indices in buffer view 1, a material with two textures, and a node tree.
constexpr std::string_view kTriangle = R"({
  "asset": {"version": "2.0"},
  "buffers": [{"byteLength": 104}],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 96},
    {"buffer": 0, "byteOffset": 96, "byteLength": 6}
  ],
  "accessors": [
    {"bufferView": 0, "byteOffset": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 0, "byteOffset": 36, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 0, "byteOffset": 72, "componentType": 5126, "count": 3, "type": "VEC2"},
    {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
  ],
  "meshes": [
    {"name": "Triangle",
     "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}
  ],
  "materials": [
    {"name": "Steel",
     "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.25, 1, 1], "metallicFactor": 0.75,
                              "roughnessFactor": 0.125, "baseColorTexture": {"index": 0}},
     "normalTexture": {"index": 1, "scale": 2, "texCoord": 0},
     "alphaMode": "MASK", "alphaCutoff": 0.25, "doubleSided": true}
  ],
  "textures": [{"source": 0}, {"source": 1}],
  "images": [{"uri": "Textures/Rock%20Color.png"}, {"uri": "data:image/png;base64,AAAA"}],
  "nodes": [{"name": "Root", "children": [1]}, {"mesh": 0}],
  "scenes": [{"nodes": [0]}]
})";

constexpr std::array<float, 9> kPositions = {0, 0, 0, 1, 0, 0, 0, 1, 0};

template <typename T>
void append(std::vector<std::byte>& bytes, std::span<const T> values) {
    const auto* data = reinterpret_cast<const std::byte*>(values.data());
    bytes.insert(bytes.end(), data, data + values.size_bytes());
}

// The 104-byte buffer kTriangle describes.
std::vector<std::byte> triangleBuffer() {
    constexpr std::array<float, 9> kNormals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    constexpr std::array<float, 6> kTexCoords = {0, 0, 1, 0, 0, 1};
    constexpr std::array<std::uint16_t, 4> kIndices = {0, 1, 2, 0};
    std::vector<std::byte> bytes;
    append<float>(bytes, kPositions);
    append<float>(bytes, kNormals);
    append<float>(bytes, kTexCoords);
    append<std::uint16_t>(bytes, kIndices);
    bytes.resize(104);
    return bytes;
}

std::string base64(std::span<const std::byte> bytes) {
    constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        std::uint32_t group = std::to_integer<std::uint32_t>(bytes[index]) << 16U;
        if (index + 1 < bytes.size()) {
            group |= std::to_integer<std::uint32_t>(bytes[index + 1]) << 8U;
        }
        if (index + 2 < bytes.size()) {
            group |= std::to_integer<std::uint32_t>(bytes[index + 2]);
        }
        text += kAlphabet[(group >> 18U) & 63U];
        text += kAlphabet[(group >> 12U) & 63U];
        text += index + 1 < bytes.size() ? kAlphabet[(group >> 6U) & 63U] : '=';
        text += index + 2 < bytes.size() ? kAlphabet[group & 63U] : '=';
    }
    return text;
}

Value parse(std::string_view text) {
    auto value = serialization::parseJson(text);
    REQUIRE(value.hasValue());
    return value ? std::move(*value) : Value();
}

std::string json(const Value& value) {
    auto text = serialization::writeJson(value);
    REQUIRE(text.hasValue());
    return text ? *text : std::string();
}

// Element `index` of the top-level array `key` of `document`.
Value& at(Value& document, std::string_view key, std::size_t index) {
    Value* array = document.find(key);
    REQUIRE(array != nullptr);
    REQUIRE(index < array->elements().size());
    return array->elements()[index];
}

// kTriangle with its buffer as a base64 data: URI.
Value embeddedTriangle() {
    Value document = parse(kTriangle);
    at(document, "buffers", 0)
        .set("uri", Value::string("data:application/octet-stream;base64," + base64(triangleBuffer())));
    return document;
}

// A GLB file of `document` and `bin`.
std::string glb(const std::string& documentText, std::span<const std::byte> bin) {
    std::string jsonChunk = documentText;
    jsonChunk.resize((jsonChunk.size() + 3) / 4 * 4, ' ');
    std::vector<std::byte> binChunk(bin.begin(), bin.end());
    binChunk.resize((binChunk.size() + 3) / 4 * 4);
    std::string file;
    const auto put = [&file](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            file += static_cast<char>((value >> shift) & 0xFFU);
        }
    };
    const std::size_t total = 12 + 8 + jsonChunk.size() + (binChunk.empty() ? 0 : 8 + binChunk.size());
    put(0x46546C67U);
    put(2);
    put(static_cast<std::uint32_t>(total));
    put(static_cast<std::uint32_t>(jsonChunk.size()));
    put(0x4E4F534AU);
    file += jsonChunk;
    if (!binChunk.empty()) {
        put(static_cast<std::uint32_t>(binChunk.size()));
        put(0x004E4942U);
        file.append(reinterpret_cast<const char*>(binChunk.data()), binChunk.size());
    }
    return file;
}

std::span<const std::byte> bytesOf(std::string_view text) {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

ith::Expected<ith::ImportOutput> runImport(std::string_view source, std::span<const ith::ImportFile> files = {}) {
    const ith::GltfImporter importer;
    return importer.import(bytesOf(source), files, importer.defaultSettings());
}

ith::ImportOutput importOk(std::string_view source, std::span<const ith::ImportFile> files = {}) {
    ith::Expected<ith::ImportOutput> output = runImport(source, files);
    REQUIRE_MESSAGE(output.hasValue(), (output ? "" : output.error().message));
    return output ? std::move(*output) : ith::ImportOutput{};
}

std::string errorCode(std::string_view source, std::span<const ith::ImportFile> files = {}) {
    const ith::Expected<ith::ImportOutput> output = runImport(source, files);
    if (output) {
        return "ok";
    }
    return output.error().code;
}

template <typename T>
T decode(std::span<const std::byte> payload, const serialization::DocumentFormat& format) {
    T value;
    const auto read = serialization::readBinary(payload, format, value);
    REQUIRE_MESSAGE(read.hasValue(), (read ? "" : read.error().toString()));
    return value;
}

ith::ImportedMesh meshOf(const ith::ImportedObject& object) {
    CHECK(object.type == "mesh");
    return decode<ith::ImportedMesh>(object.payload, ith::kImportedMeshFormat);
}

std::vector<std::string> namesOf(const ith::ImportOutput& output) {
    std::vector<std::string> names;
    for (const ith::ImportedObject& object : output.objects) {
        names.push_back(object.type + " " + object.name);
    }
    return names;
}

// Checks that `output` holds the triangle of kTriangle.
void checkTriangle(const ith::ImportOutput& output) {
    REQUIRE(output.objects.size() == 2);
    const ith::ImportedMesh mesh = meshOf(output.objects[0]);
    CHECK(output.objects[0].name == "Triangle");
    REQUIRE(mesh.primitives.size() == 1);
    const ith::ImportedPrimitive& primitive = mesh.primitives[0];
    CHECK(primitive.positions == std::vector<float>(kPositions.begin(), kPositions.end()));
    CHECK(primitive.normals == std::vector<float>{0, 0, 1, 0, 0, 1, 0, 0, 1});
    CHECK(primitive.texCoords == std::vector<float>{0, 0, 1, 0, 0, 1});
    CHECK(primitive.indices == std::vector<std::uint32_t>{0, 1, 2});
    CHECK(primitive.material == "material:Steel");

    CHECK(output.objects[1].type == "material");
    CHECK(output.objects[1].name == "Steel");
    const auto material = decode<ith::ImportedMaterial>(output.objects[1].payload, ith::kImportedMaterialFormat);
    CHECK(material.baseColorFactor == std::array<float, 4>{0.5F, 0.25F, 1.0F, 1.0F});
    CHECK(material.metallicFactor == 0.75F);
    CHECK(material.roughnessFactor == 0.125F);
    CHECK(material.alphaMode == ith::ImportedAlphaMode::Mask);
    CHECK(material.alphaCutoff == 0.25F);
    CHECK(material.doubleSided);
    REQUIRE(material.baseColorTexture.has_value());
    CHECK(material.baseColorTexture.value_or(ith::ImportedTexture{}).image == "Textures/Rock Color.png");
    REQUIRE(material.normalTexture.has_value());
    CHECK(material.normalTexture.value_or(ith::ImportedTexture{.image = "x"}).image.empty()); // embedded
    CHECK(material.normalScale == 2.0F);
    CHECK_FALSE(material.metallicRoughnessTexture.has_value());
    CHECK(material.emissiveFactor == std::array<float, 3>{0.0F, 0.0F, 0.0F});

    CHECK(output.references == std::vector<std::string>{"Textures/Rock Color.png"});
}

// The sub-asset ids of an import by key.
std::map<std::string, ith::SubAssetId> idsOf(const ith::ImportReport& report) {
    std::map<std::string, ith::SubAssetId> ids;
    for (const ith::ImportedSubAsset& subAsset : report.subAssets) {
        ids.emplace(subAsset.key, subAsset.id);
    }
    return ids;
}

// A project with the glTF importer and a database over it.
struct Fixture {
    ith::test::TempProject project;
    ith::ImporterRegistry importers;
    std::unique_ptr<ith::AssetDatabase> database;

    Fixture() {
        importers.add(std::make_unique<ith::GltfImporter>());
        database = std::make_unique<ith::AssetDatabase>(project.directory(), importers);
    }

    ith::ImportReport import(std::string_view path) {
        ith::Expected<ith::ImportReport> report = database->import(path);
        REQUIRE_MESSAGE(report.hasValue(), (report ? "" : report.error().message));
        return report ? std::move(*report) : ith::ImportReport{};
    }
};

std::string_view textOf(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace

TEST_SUITE("asset_importers.gltf") {
    TEST_CASE("the importer's identity") {
        const ith::GltfImporter importer;
        CHECK(importer.id() == "gltf");
        CHECK(importer.version() >= 1);
        const auto extensions = importer.extensions();
        CHECK(std::vector<std::string_view>(extensions.begin(), extensions.end()) ==
              std::vector<std::string_view>{".gltf", ".glb"});
        CHECK(importer.defaultSettings() == Value::object());
    }

    TEST_CASE("a .gltf file with an embedded base64 buffer") {
        const std::string source = json(embeddedTriangle());
        const ith::GltfImporter importer;
        const auto dependencies = importer.dependencies(bytesOf(source), importer.defaultSettings());
        REQUIRE(dependencies.hasValue());
        CHECK(dependencies.valueOr(std::vector<std::string>{"x"}).empty());
        checkTriangle(importOk(source));

        // Padding is optional.
        Value document = parse(kTriangle);
        std::string data = base64(triangleBuffer());
        while (!data.empty() && data.back() == '=') {
            data.pop_back();
        }
        at(document, "buffers", 0).set("uri", Value::string("DATA:application/gltf-buffer;BASE64," + data));
        checkTriangle(importOk(json(document)));
    }

    TEST_CASE("a .gltf file with an external buffer next to it") {
        Value document = parse(kTriangle);
        at(document, "buffers", 0).set("uri", Value::string("Data/Tri%20angle.bin"));
        const std::string source = json(document);
        const ith::GltfImporter importer;
        const auto dependencies = importer.dependencies(bytesOf(source), importer.defaultSettings());
        CHECK(dependencies.valueOr(std::vector<std::string>{}) == std::vector<std::string>{"Data/Tri angle.bin"});

        const std::vector<std::byte> buffer = triangleBuffer();
        const std::array<ith::ImportFile, 1> files = {ith::ImportFile{.path = "Data/Tri angle.bin", .bytes = buffer}};
        checkTriangle(importOk(source, files));
        // A buffer longer than its byteLength is fine; a shorter one is not.
        std::vector<std::byte> longer = buffer;
        longer.resize(200);
        checkTriangle(importOk(source, std::array{ith::ImportFile{.path = "Data/Tri angle.bin", .bytes = longer}}));
        CHECK(errorCode(source, std::array{ith::ImportFile{.path = "Data/Tri angle.bin",
                                                           .bytes = std::span(buffer).first(100)}}) ==
              "import.malformed");
        // A file that wasn't passed.
        CHECK(errorCode(source) == "import.malformed");
    }

    TEST_CASE("a .glb file with its buffer in the BIN chunk") {
        const std::vector<std::byte> buffer = triangleBuffer();
        const std::string file = glb(std::string(kTriangle), buffer);
        const ith::GltfImporter importer;
        CHECK(importer.dependencies(bytesOf(file), importer.defaultSettings())
                  .valueOr(std::vector<std::string>{"x"})
                  .empty());
        checkTriangle(importOk(file));

        // Unknown chunks after the BIN chunk are skipped.
        std::string extra = glb(std::string(kTriangle), buffer);
        extra += std::string("\x04\x00\x00\x00XTRA\x01\x02\x03\x04", 12);
        const auto length = static_cast<std::uint32_t>(extra.size());
        std::memcpy(extra.data() + 8, &length, 4);
        checkTriangle(importOk(extra));

        // A GLB file may also use an embedded buffer and no BIN chunk.
        checkTriangle(importOk(glb(json(embeddedTriangle()), {})));
    }

    TEST_CASE("interleaved, normalized, sparse and zero-filled accessors") {
        // Two vertices' worth of interleaved data per element: position (12 bytes), uv as two
        // normalized unsigned shorts (4 bytes): stride 16. Indices are unsigned bytes.
        std::vector<std::byte> buffer;
        const std::array<std::array<float, 3>, 3> positions = {{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}}};
        const std::array<std::array<std::uint16_t, 2>, 3> uvs = {{{0, 0}, {65535, 0}, {0, 32768}}};
        for (std::size_t vertex = 0; vertex < 3; ++vertex) {
            append<float>(buffer, positions[vertex]);
            append<std::uint16_t>(buffer, uvs[vertex]);
        }
        const std::array<std::uint8_t, 4> indices = {2, 1, 0, 0};
        append<std::uint8_t>(buffer, indices); // at 48
        // Sparse: replace normal 1 of a zero-filled accessor with (0, 0, 1); index at 52, value at 56.
        const std::array<std::uint32_t, 1> sparseIndex = {1};
        const std::array<float, 3> sparseValue = {0, 0, 1};
        append<std::uint32_t>(buffer, sparseIndex);
        append<float>(buffer, sparseValue);
        REQUIRE(buffer.size() == 68);

        Value document = parse(R"({
          "asset": {"version": "2.1", "minVersion": "2.0"},
          "extensionsUsed": ["KHR_materials_emissive_strength"],
          "buffers": [{"byteLength": 68}],
          "bufferViews": [
            {"buffer": 0, "byteLength": 48, "byteStride": 16},
            {"buffer": 0, "byteOffset": 48, "byteLength": 3},
            {"buffer": 0, "byteOffset": 52, "byteLength": 4},
            {"buffer": 0, "byteOffset": 56, "byteLength": 12}
          ],
          "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [9, 9, 9]},
            {"bufferView": 0, "byteOffset": 12, "componentType": 5123, "normalized": true, "count": 3, "type": "VEC2"},
            {"bufferView": 1, "componentType": 5121, "count": 3, "type": "SCALAR"},
            {"componentType": 5126, "count": 3, "type": "VEC3",
             "sparse": {"count": 1, "indices": {"bufferView": 2, "componentType": 5125},
                        "values": {"bufferView": 3}}}
          ],
          "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1, "NORMAL": 3, "COLOR_0": 7},
                                      "indices": 2}]}]
        })");
        at(document, "buffers", 0).set("uri", Value::string("data:;base64," + base64(buffer)));
        const ith::ImportOutput output = importOk(json(document));
        REQUIRE(output.objects.size() == 1);
        const ith::ImportedMesh mesh = meshOf(output.objects[0]);
        REQUIRE(mesh.primitives.size() == 1);
        const ith::ImportedPrimitive& primitive = mesh.primitives[0];
        CHECK(primitive.positions == std::vector<float>{0, 0, 0, 2, 0, 0, 0, 2, 0});
        CHECK(primitive.texCoords == std::vector<float>{0, 0, 1, 0, 0, 32768.0F / 65535.0F});
        CHECK(primitive.normals == std::vector<float>{0, 0, 0, 0, 0, 1, 0, 0, 0});
        CHECK(primitive.indices == std::vector<std::uint32_t>{2, 1, 0});
        CHECK(primitive.material.empty());
    }

    TEST_CASE("a primitive without indices gets 0, 1, 2, ...") {
        Value document = embeddedTriangle();
        at(document, "meshes", 0).find("primitives")->elements()[0].remove("indices");
        const ith::ImportedMesh mesh = meshOf(importOk(json(document)).objects[0]);
        REQUIRE(mesh.primitives.size() == 1);
        CHECK(mesh.primitives[0].indices == std::vector<std::uint32_t>{0, 1, 2});
    }

    TEST_CASE("meshes are named by their own name, else by the first node that uses them") {
        Value document = embeddedTriangle();
        // Mesh 0 "Triangle"; mesh 1 unnamed, used by "Body" under "Root" through an unnamed node;
        // mesh 2 unnamed and unused; mesh 3 named like mesh 0.
        Value* meshes = document.find("meshes");
        const Value unnamed = [&] {
            Value copy = meshes->elements()[0];
            copy.remove("name");
            return copy;
        }();
        const Value triangle = meshes->elements()[0];
        meshes->elements().push_back(unnamed);
        meshes->elements().push_back(unnamed);
        meshes->elements().push_back(triangle);
        document.set("nodes", parse(R"([
          {"name": "Root", "children": [1]},
          {"children": [2, 3]},
          {"name": "Body", "mesh": 1},
          {"name": "Later", "mesh": 1}
        ])"));
        // Two materials named alike, and one unnamed.
        Value* materials = document.find("materials");
        const Value steel = materials->elements()[0];
        materials->elements().push_back(steel);
        materials->elements().push_back(Value::object());

        const ith::ImportOutput output = importOk(json(document));
        CHECK(namesOf(output) == std::vector<std::string>{"mesh Triangle", "mesh Root/Body", "mesh ", "mesh Triangle",
                                                          "material Steel", "material Steel", "material "});
        const auto keys = ith::sourceKeys(output.objects);
        CHECK(keys.valueOr(std::vector<std::string>{}) ==
              std::vector<std::string>{"mesh:Triangle", "mesh:Root/Body", "mesh:#0", "mesh:Triangle#1",
                                       "material:Steel", "material:Steel#1", "material:#0"});
        // The unnamed material's defaults.
        const auto material = decode<ith::ImportedMaterial>(output.objects[6].payload, ith::kImportedMaterialFormat);
        CHECK(material.baseColorFactor == std::array<float, 4>{1.0F, 1.0F, 1.0F, 1.0F});
        CHECK(material.alphaMode == ith::ImportedAlphaMode::Opaque);
        CHECK_FALSE(material.baseColorTexture.has_value());
    }

    TEST_CASE("reimport keeps ids when meshes, materials and nodes are reordered") {
        Fixture fixture;
        // "Hull" is named by itself; the unnamed mesh by its node "Ship/Sail".
        Value document = embeddedTriangle();
        Value* meshes = document.find("meshes");
        meshes->elements()[0].set("name", Value::string("Hull"));
        Value sail = meshes->elements()[0];
        sail.remove("name");
        sail.find("primitives")->elements()[0].set("material", Value::integer(1));
        meshes->elements().push_back(sail);
        Value* materials = document.find("materials");
        Value paint = Value::object();
        paint.set("name", Value::string("Paint"));
        materials->elements().push_back(paint);
        document.set("nodes",
                     parse(R"([{"name": "Ship", "children": [1, 2]}, {"mesh": 0}, {"name": "Sail", "mesh": 1}])"));
        fixture.project.write("Ships/Ship.gltf", json(document));
        const ith::ImportReport first = fixture.import("Ships/Ship.gltf");
        CHECK(first.created);
        const std::map<std::string, ith::SubAssetId> before = idsOf(first);
        CHECK(before.size() == 4);
        CHECK(before.contains("mesh:Ship/Sail"));
        CHECK(first.references == std::vector<std::string>{"Ships/Textures/Rock Color.png"});

        // Re-exported with every array in another order.
        std::ranges::reverse(meshes->elements());
        std::ranges::reverse(materials->elements());
        for (Value& mesh : meshes->elements()) {
            Value& primitive = mesh.find("primitives")->elements()[0];
            primitive.set("material", Value::integer(1 - primitive.find("material")->toNumber<int>().value_or(0)));
        }
        document.set("nodes",
                     parse(R"([{"name": "Sail", "mesh": 0}, {"mesh": 1}, {"name": "Ship", "children": [1, 0]}])"));
        fixture.project.write("Ships/Ship.gltf", json(document));
        const ith::ImportReport reordered = fixture.import("Ships/Ship.gltf");
        CHECK_FALSE(reordered.fromCache);
        CHECK(idsOf(reordered) == before);
        CHECK(reordered.missing.empty());
        // The primitives still point at the same materials by key.
        for (const ith::ImportedSubAsset& subAsset : reordered.subAssets) {
            if (subAsset.key.starts_with("mesh:")) {
                const auto mesh = decode<ith::ImportedMesh>(subAsset.payload, ith::kImportedMeshFormat);
                REQUIRE(mesh.primitives.size() == 1);
                CHECK(mesh.primitives[0].material ==
                      (subAsset.key == "mesh:Hull" ? "material:Steel" : "material:Paint"));
            }
        }

        // Renaming the node of the unnamed mesh changes its key: a new id, the old one missing.
        document.set("nodes",
                     parse(R"([{"name": "Jib", "mesh": 0}, {"mesh": 1}, {"name": "Ship", "children": [1, 0]}])"));
        fixture.project.write("Ships/Ship.gltf", json(document));
        const ith::ImportReport renamed = fixture.import("Ships/Ship.gltf");
        CHECK(idsOf(renamed).at("mesh:Hull") == before.at("mesh:Hull"));
        CHECK(idsOf(renamed).contains("mesh:Ship/Jib"));
        REQUIRE(renamed.missing.size() == 1);
        CHECK(renamed.missing[0].id == before.at("mesh:Ship/Sail"));
    }

    TEST_CASE("the derived-data cache round trip with an external buffer") {
        Fixture fixture;
        Value document = parse(kTriangle);
        at(document, "buffers", 0).set("uri", Value::string("Triangle.bin"));
        const std::vector<std::byte> buffer = triangleBuffer();
        fixture.project.write("Meshes/Triangle.gltf", json(document));
        fixture.project.write("Meshes/Triangle.bin", textOf(buffer));

        const ith::ImportReport first = fixture.import("Meshes/Triangle.gltf");
        CHECK_FALSE(first.fromCache);
        const ith::ImportReport cached = fixture.import("Meshes/Triangle.gltf");
        CHECK(cached.fromCache);
        REQUIRE(cached.subAssets.size() == first.subAssets.size());
        for (std::size_t index = 0; index < first.subAssets.size(); ++index) {
            CHECK(cached.subAssets[index].key == first.subAssets[index].key);
            CHECK(cached.subAssets[index].id == first.subAssets[index].id);
            CHECK(cached.subAssets[index].payload == first.subAssets[index].payload);
        }
        CHECK(cached.references == std::vector<std::string>{"Meshes/Textures/Rock Color.png"});
        const auto mesh = decode<ith::ImportedMesh>(cached.subAssets[0].payload, ith::kImportedMeshFormat);
        REQUIRE(mesh.primitives.size() == 1);
        CHECK(mesh.primitives[0].indices == std::vector<std::uint32_t>{0, 1, 2});

        // A changed buffer is a miss, with the same ids.
        std::vector<std::byte> moved = buffer;
        const float two = 2.0F;
        std::memcpy(moved.data() + 12, &two, sizeof(two));
        fixture.project.write("Meshes/Triangle.bin", textOf(moved));
        const ith::ImportReport changed = fixture.import("Meshes/Triangle.gltf");
        CHECK_FALSE(changed.fromCache);
        CHECK(idsOf(changed) == idsOf(first));
        const auto changedMesh = decode<ith::ImportedMesh>(changed.subAssets[0].payload, ith::kImportedMeshFormat);
        REQUIRE(changedMesh.primitives.size() == 1);
        CHECK(changedMesh.primitives[0].positions[3] == 2.0F);

        // A missing buffer fails the import.
        std::error_code removeError;
        std::filesystem::remove(fixture.project.content("Meshes/Triangle.bin"), removeError);
        REQUIRE_FALSE(removeError);
        const auto missing = fixture.database->import("Meshes/Triangle.gltf");
        REQUIRE_FALSE(missing.hasValue());
        CHECK(missing.error().code == "asset.not_found");
    }

    TEST_CASE("a .glb file through the asset database") {
        Fixture fixture;
        fixture.project.write("Triangle.glb", glb(std::string(kTriangle), triangleBuffer()));
        const ith::ImportReport report = fixture.import("Triangle.glb");
        CHECK(report.subAssets.size() == 2);
        CHECK(report.references == std::vector<std::string>{"Textures/Rock Color.png"});
        CHECK(fixture.import("Triangle.glb").fromCache);
    }

    TEST_CASE("malformed GLB containers") {
        const std::vector<std::byte> buffer = triangleBuffer();
        const std::string valid = glb(std::string(kTriangle), buffer);
        const auto withU32 = [&](std::size_t offset, std::uint32_t value) {
            std::string file = valid;
            std::memcpy(file.data() + offset, &value, 4);
            return file;
        };
        const std::size_t jsonLength = (kTriangle.size() + 3) / 4 * 4;
        const std::size_t binHeader = 20 + jsonLength;

        CHECK(errorCode(valid.substr(0, 8)) == "import.malformed");                // truncated header
        CHECK(errorCode(valid.substr(0, valid.size() - 4)) == "import.malformed"); // length mismatch
        CHECK(errorCode(withU32(4, 1)) == "import.unsupported");                   // version 1
        CHECK(errorCode(withU32(8, 4096)) == "import.malformed");                  // header length
        CHECK(errorCode(withU32(12, 0xFFFFFFF0U)) == "import.malformed");          // JSON chunk too long
        CHECK(errorCode(withU32(12, static_cast<std::uint32_t>(jsonLength - 2))) == "import.malformed"); // unaligned
        CHECK(errorCode(withU32(16, 0x004E4942U)) == "import.malformed");            // first chunk not JSON
        CHECK(errorCode(withU32(binHeader, 0x7FFFFFFCU)) == "import.malformed");     // BIN chunk too long
        CHECK(errorCode(withU32(binHeader + 4, 0x4E4F534AU)) == "import.malformed"); // second JSON chunk
        // A truncated chunk header, and a file of just the header.
        std::string truncated = valid.substr(0, binHeader + 4);
        const auto truncatedLength = static_cast<std::uint32_t>(truncated.size());
        std::memcpy(truncated.data() + 8, &truncatedLength, 4);
        CHECK(errorCode(truncated) == "import.malformed");
        std::string headerOnly = valid.substr(0, 12);
        const std::uint32_t twelve = 12;
        std::memcpy(headerOnly.data() + 8, &twelve, 4);
        CHECK(errorCode(headerOnly) == "import.malformed");
        // The BIN chunk is shorter than the buffer's byteLength.
        CHECK(errorCode(glb(std::string(kTriangle), std::span(buffer).first(64))) == "import.malformed");
        // No BIN chunk and no uri.
        CHECK(errorCode(glb(std::string(kTriangle), {})) == "import.malformed");
    }

    TEST_CASE("malformed documents") {
        CHECK(errorCode("") == "import.malformed");
        CHECK(errorCode("[1, 2]") == "import.malformed");
        CHECK(errorCode("{}") == "import.malformed");
        CHECK(errorCode(R"({"asset": {}})") == "import.malformed");
        CHECK(errorCode(R"({"asset": {"version": "1.0"}})") == "import.unsupported");
        CHECK(errorCode(R"({"asset": {"version": "2.0", "minVersion": "2.1"}})") == "import.unsupported");
        CHECK(errorCode(R"({"asset": {"version": "2.0"}, "extensionsRequired": ["KHR_draco_mesh_compression"]})") ==
              "import.unsupported");
        CHECK(errorCode(R"({"asset": {"version": "2.0"}, "meshes": {}})") == "import.malformed");
        CHECK(errorCode(R"({"asset": {"version": "2.0"}})") == "ok");
    }

    TEST_CASE("buffer, view and accessor bounds") {
        struct Case {
            const char* name;
            std::string_view array;
            std::size_t index;
            std::string_view key;
            Value value;
            std::string_view code;
        };

        const std::uint64_t huge = std::uint64_t{1} << 62U;
        const std::vector<Case> cases = {
            {"buffer byteLength larger than the data", "buffers", 0, "byteLength", Value::integer(200),
             "import.malformed"},
            {"buffer byteLength 0", "buffers", 0, "byteLength", Value::integer(0), "import.malformed"},
            {"view past its buffer", "bufferViews", 1, "byteLength", Value::integer(9), "import.malformed"},
            {"view offset overflow", "bufferViews", 1, "byteOffset", Value::unsignedInteger(~std::uint64_t{0}),
             "import.malformed"},
            {"view of a missing buffer", "bufferViews", 0, "buffer", Value::integer(3), "import.malformed"},
            {"stride too small", "bufferViews", 0, "byteStride", Value::integer(8), "import.malformed"},
            {"stride not a multiple of 4", "bufferViews", 0, "byteStride", Value::integer(14), "import.malformed"},
            {"stride too large", "bufferViews", 0, "byteStride", Value::integer(256), "import.malformed"},
            {"stride that walks past the view", "bufferViews", 0, "byteStride", Value::integer(252),
             "import.malformed"},
            {"accessor count past the view", "accessors", 0, "count", Value::integer(9), "import.malformed"},
            {"accessor count that overflows", "accessors", 0, "count", Value::unsignedInteger(huge),
             "import.malformed"},
            {"accessor offset past the view", "accessors", 2, "byteOffset", Value::integer(80), "import.malformed"},
            {"misaligned accessor", "accessors", 0, "byteOffset", Value::integer(2), "import.malformed"},
            {"negative count", "accessors", 0, "count", Value::integer(-1), "import.malformed"},
            {"fractional count", "accessors", 0, "count", Value::number("1.5"), "import.malformed"},
            {"count 0", "accessors", 0, "count", Value::integer(0), "import.malformed"},
            {"unknown component type", "accessors", 0, "componentType", Value::integer(5124), "import.malformed"},
            {"integer positions", "accessors", 0, "componentType", Value::integer(5125), "import.unsupported"},
            {"wrong type", "accessors", 0, "type", Value::string("VEC2"), "import.malformed"},
            {"unknown type", "accessors", 0, "type", Value::string("VEC5"), "import.malformed"},
            {"normalized floats", "accessors", 0, "normalized", Value::boolean(true), "import.malformed"},
            {"missing view", "accessors", 0, "bufferView", Value::integer(7), "import.malformed"},
            {"normal count differs", "accessors", 1, "count", Value::integer(2), "import.malformed"},
            {"index count not a multiple of 3", "accessors", 3, "count", Value::integer(2), "import.malformed"},
            {"float indices", "accessors", 3, "componentType", Value::integer(5126), "import.malformed"},
        };
        for (const Case& testCase : cases) {
            CAPTURE(testCase.name);
            Value document = embeddedTriangle();
            at(document, testCase.array, testCase.index).set(std::string(testCase.key), testCase.value);
            CHECK(errorCode(json(document)) == testCase.code);
        }
    }

    TEST_CASE("an accessor without a view is zeros, within the decode budget") {
        Value document = embeddedTriangle();
        Value& positions = at(document, "accessors", 0);
        positions.remove("bufferView");
        positions.remove("byteOffset");
        positions.set("count", Value::unsignedInteger(std::uint64_t{1} << 40U));
        at(document, "accessors", 1).set("count", Value::unsignedInteger(std::uint64_t{1} << 40U));
        CHECK(errorCode(json(document)) == "import.too_large");

        positions.set("byteOffset", Value::integer(4));
        CHECK(errorCode(json(document)) == "import.malformed");
    }

    TEST_CASE("index range and primitive checks") {
        // Index 3 of a 3-vertex primitive.
        std::vector<std::byte> buffer = triangleBuffer();
        const std::uint16_t three = 3;
        std::memcpy(buffer.data() + 100, &three, 2);
        Value document = parse(kTriangle);
        at(document, "buffers", 0).set("uri", Value::string("data:;base64," + base64(buffer)));
        CHECK(errorCode(json(document)) == "import.malformed");

        const auto primitiveCase = [](std::string_view key, Value value) {
            Value changed = embeddedTriangle();
            at(changed, "meshes", 0).find("primitives")->elements()[0].set(std::string(key), std::move(value));
            return errorCode(json(changed));
        };
        CHECK(primitiveCase("mode", Value::integer(1)) == "import.unsupported");
        CHECK(primitiveCase("mode", Value::integer(5)) == "import.unsupported");
        CHECK(primitiveCase("mode", Value::integer(4)) == "ok");
        CHECK(primitiveCase("material", Value::integer(1)) == "import.malformed");
        CHECK(primitiveCase("indices", Value::integer(9)) == "import.malformed");
        CHECK(primitiveCase("attributes", parse(R"({"NORMAL": 1})")) == "import.malformed");
        CHECK(primitiveCase("attributes", Value::integer(1)) == "import.malformed");

        Value noPrimitives = embeddedTriangle();
        at(noPrimitives, "meshes", 0).set("primitives", Value::array());
        CHECK(errorCode(json(noPrimitives)) == "import.malformed");

        // Without indices, the vertex count must make whole triangles.
        Value noIndices = embeddedTriangle();
        at(noIndices, "meshes", 0).find("primitives")->elements()[0].remove("indices");
        at(noIndices, "accessors", 0).set("count", Value::integer(2));
        at(noIndices, "accessors", 1).set("count", Value::integer(2));
        at(noIndices, "accessors", 2).set("count", Value::integer(2));
        CHECK(errorCode(json(noIndices)) == "import.malformed");
    }

    TEST_CASE("values that aren't finite are refused") {
        std::vector<std::byte> buffer = triangleBuffer();
        const auto nan = std::bit_cast<float>(0x7FC00000U);
        std::memcpy(buffer.data() + 4, &nan, 4);
        Value document = parse(kTriangle);
        at(document, "buffers", 0).set("uri", Value::string("data:;base64," + base64(buffer)));
        CHECK(errorCode(json(document)) == "import.malformed");

        Value material = embeddedTriangle();
        at(material, "materials", 0).set("alphaCutoff", Value::string("half"));
        CHECK(errorCode(json(material)) == "import.malformed");
        at(material, "materials", 0).set("alphaCutoff", Value::number("1e999"));
        CHECK(errorCode(json(material)) == "import.malformed");
        at(material, "materials", 0).set("alphaCutoff", Value::real(0.5));
        at(material, "materials", 0).set("alphaMode", Value::string("CUTOUT"));
        CHECK(errorCode(json(material)) == "import.malformed");
        at(material, "materials", 0).set("alphaMode", Value::string("BLEND"));
        at(material, "materials", 0).set("emissiveFactor", parse("[1, 1]"));
        CHECK(errorCode(json(material)) == "import.malformed");
        at(material, "materials", 0).set("emissiveFactor", parse("[1, 0.5, 0]"));
        CHECK(errorCode(json(material)) == "ok");
        at(material, "materials", 0).set("emissiveTexture", parse(R"({"index": 5})"));
        CHECK(errorCode(json(material)) == "import.malformed");
    }

    TEST_CASE("sparse accessors are checked") {
        // Positions 0..2 with one sparse replacement; the sparse data follows the indices.
        std::vector<std::byte> buffer = triangleBuffer();
        const std::array<std::uint16_t, 2> sparseIndices = {2, 1};
        const std::array<float, 6> sparseValues = {5, 5, 5, 6, 6, 6};
        append<std::uint16_t>(buffer, sparseIndices); // at 104
        append<float>(buffer, sparseValues);          // at 108
        const auto sparseDocument = [&](std::string_view sparse) {
            Value document = parse(kTriangle);
            at(document, "buffers", 0).set("uri", Value::string("data:;base64," + base64(buffer)));
            at(document, "buffers", 0).set("byteLength", Value::integer(static_cast<std::int64_t>(buffer.size())));
            document.find("bufferViews")
                ->elements()
                .push_back(parse(R"({"buffer": 0, "byteOffset": 104, "byteLength": 4})"));
            document.find("bufferViews")
                ->elements()
                .push_back(parse(R"({"buffer": 0, "byteOffset": 108, "byteLength": 24})"));
            at(document, "accessors", 0).set("sparse", parse(sparse));
            return json(document);
        };
        const std::string_view kOne =
            R"({"count": 1, "indices": {"bufferView": 2, "byteOffset": 2, "componentType": 5123},
                                          "values": {"bufferView": 3}})";
        const ith::ImportOutput output = importOk(sparseDocument(kOne));
        CHECK(meshOf(output.objects[0]).primitives[0].positions == std::vector<float>{0, 0, 0, 5, 5, 5, 0, 1, 0});

        // Indices that don't increase, counts past the data, and wrong types.
        CHECK(errorCode(sparseDocument(R"({"count": 2, "indices": {"bufferView": 2, "componentType": 5123},
                                           "values": {"bufferView": 3}})")) == "import.malformed");
        CHECK(errorCode(sparseDocument(R"({"count": 4, "indices": {"bufferView": 2, "componentType": 5123},
                                           "values": {"bufferView": 3}})")) == "import.malformed");
        CHECK(errorCode(sparseDocument(R"({"count": 0, "indices": {"bufferView": 2, "componentType": 5123},
                                           "values": {"bufferView": 3}})")) == "import.malformed");
        CHECK(errorCode(sparseDocument(R"({"count": 1, "indices": {"bufferView": 2, "componentType": 5126},
                                           "values": {"bufferView": 3}})")) == "import.malformed");
        CHECK(errorCode(
                  sparseDocument(R"({"count": 1, "indices": {"bufferView": 2, "byteOffset": 1, "componentType": 5123},
                                           "values": {"bufferView": 3}})")) == "import.malformed");
        CHECK(errorCode(sparseDocument(R"({"count": 1, "indices": {"bufferView": 2, "componentType": 5123}})")) ==
              "import.malformed");
        CHECK(errorCode(sparseDocument(R"({"count": 1, "indices": {"bufferView": 2, "componentType": 5125},
                                           "values": {"bufferView": 3}})")) == "import.malformed"); // index 65538
        CHECK(errorCode(sparseDocument(R"({"count": 1, "indices": {"bufferView": 2, "componentType": 5123},
                                           "values": {"bufferView": 3, "byteOffset": 16}})")) == "import.malformed");
    }

    TEST_CASE("URIs that leave the file's folder or aren't paths are refused") {
        const std::vector<std::string_view> bad = {"../Triangle.bin",      "Data/../../Triangle.bin",
                                                   "%2e%2e/Triangle.bin",  "/Triangle.bin",
                                                   "C:/Triangle.bin",      "C%3A/Triangle.bin",
                                                   "Data\\Triangle.bin",   "Data%5CTriangle.bin",
                                                   "file:///Triangle.bin", "http://example.com/Triangle.bin",
                                                   "Triangle.bin?x=1",     "Triangle.bin#x",
                                                   "./Triangle.bin",       "Data//Triangle.bin",
                                                   "Triangle%2",           "Triangle%zz.bin",
                                                   "Tri%00angle.bin",      "",
                                                   "Triangle.bin.meta"};
        const ith::GltfImporter importer;
        for (const std::string_view uri : bad) {
            CAPTURE(uri);
            Value document = parse(kTriangle);
            at(document, "buffers", 0).set("uri", Value::string(std::string(uri)));
            const std::string source = json(document);
            const auto dependencies = importer.dependencies(bytesOf(source), importer.defaultSettings());
            REQUIRE_FALSE(dependencies.hasValue());
            CHECK(dependencies.error().code == "import.invalid_uri");
            CHECK(errorCode(source) == "import.invalid_uri");

            // The same rules hold for images.
            Value image = embeddedTriangle();
            at(image, "images", 0).set("uri", Value::string(std::string(uri)));
            CHECK(errorCode(json(image)) == "import.invalid_uri");
        }
        // Data URIs must be base64 and valid.
        for (const std::string_view uri :
             {"data:text/plain,hello", "data:;base64,A", "data:;base64,AA=A", "data:;base64,AA*A", "data:;base64",
              "data:;base64,AB==", "data:;base64,AAB="}) {
            CAPTURE(uri);
            Value document = parse(kTriangle);
            at(document, "buffers", 0).set("uri", Value::string(std::string(uri)));
            CHECK(errorCode(json(document)) == "import.invalid_uri");
        }
    }

    TEST_CASE("node trees are checked") {
        const auto withNodes = [](std::string_view nodes) {
            Value document = embeddedTriangle();
            document.set("nodes", parse(nodes));
            return errorCode(json(document));
        };
        CHECK(withNodes(R"([{"children": [1]}, {"children": [0]}])") == "import.malformed");     // cycle
        CHECK(withNodes(R"([{"children": [0]}])") == "import.malformed");                        // itself
        CHECK(withNodes(R"([{"children": [2]}, {"children": [2]}, {}])") == "import.malformed"); // two parents
        CHECK(withNodes(R"([{"children": [5]}])") == "import.malformed");                        // no such node
        CHECK(withNodes(R"([{"mesh": 3}])") == "import.malformed");                              // no such mesh
        CHECK(withNodes(R"([{"name": 7}])") == "import.malformed"); // every node's name must be a string
        CHECK(withNodes(R"([{"name": 7, "mesh": 0}])") == "import.malformed");
        CHECK(withNodes(R"([{"name": ""}])") == "ok");
        CHECK(withNodes(R"([1])") == "import.malformed");
        CHECK(withNodes(R"([{"children": [1]}, {"children": [2]}, {"children": [3]}, {}])") == "ok");
    }
}

TEST_SUITE("asset_importers.gltf_limits") {
    // Copies of one long string (an image path, a material key, node paths) could make a small
    // file build an output many times its size; every copy counts against the decode budget.
    TEST_CASE("an image path used by many texture slots counts every copy") {
        const auto document = [](std::size_t uriSize, std::size_t materials) {
            std::string uri;
            while (uri.size() + 6 < uriSize) {
                uri += std::string(std::min<std::size_t>(200, uriSize - uri.size() - 6), 'a') + "/";
            }
            uri += "x.png";
            std::string text = R"({"asset": {"version": "2.0"}, "images": [{"uri": ")" + uri +
                               R"("}], "textures": [{"source": 0}], "materials": [)";
            const std::string material =
                R"({"normalTexture": {"index": 0}, "occlusionTexture": {"index": 0}, "emissiveTexture": {"index": 0},
                    "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicRoughnessTexture": {"index": 0}}})";
            for (std::size_t index = 0; index < materials; ++index) {
                text += (index == 0 ? "" : ",") + material;
            }
            return text + "]}";
        };
        const ith::ImportOutput small = importOk(document(4000, 10));
        CHECK(small.references.size() == 1);
        CHECK(small.objects.size() == 10);
        // About 6 MB of JSON asking for 5 copies of a 4 KB path per material: about 1.2 GB.
        CHECK(errorCode(document(4000, 60000)) == "import.too_large");
        // Paths are at most kGltfMaxNameSize bytes.
        CHECK(errorCode(document(ith::kGltfMaxNameSize + 1, 1)) == "import.invalid_uri");
    }

    TEST_CASE("a material key used by many primitives counts every copy") {
        const auto document = [](std::size_t nameSize, std::size_t primitives) {
            std::string text = R"({"asset": {"version": "2.0"}, "materials": [{"name": ")" +
                               std::string(nameSize, 'm') +
                               R"("}], "accessors": [{"componentType": 5126, "count": 3, "type": "VEC3"}],
                                  "meshes": [{"primitives": [)";
            for (std::size_t index = 0; index < primitives; ++index) {
                text += std::string(index == 0 ? "" : ",") + R"({"attributes": {"POSITION": 0}, "material": 0})";
            }
            return text + "]}]}";
        };
        const ith::ImportOutput small = importOk(document(4000, 5));
        REQUIRE(small.objects.size() == 2);
        const ith::ImportedMesh mesh = meshOf(small.objects[0]);
        REQUIRE(mesh.primitives.size() == 5);
        CHECK(mesh.primitives[4].material == "material:" + std::string(4000, 'm'));
        CHECK(mesh.primitives[4].positions == std::vector<float>(9, 0.0F)); // one decode, shared
        // About 1.4 MB of JSON asking for a 4 KB key in each of 30000 primitives: about 120 MB.
        CHECK(errorCode(document(4000, 30000)) == "import.too_large");
        // Names are at most kGltfMaxNameSize bytes.
        CHECK(errorCode(document(ith::kGltfMaxNameSize + 1, 1)) == "import.too_large");
    }

    TEST_CASE("node paths are built once per node and bounded") {
        // A chain of unnamed nodes, each with its own unnamed mesh: every path is built once.
        const auto chain = [](std::size_t nodes, std::string_view name, bool sharedMesh = false) {
            std::string text = R"({"asset": {"version": "2.0"},
                "accessors": [{"componentType": 5126, "count": 3, "type": "VEC3"}], "meshes": [)";
            for (std::size_t index = 0; index < (sharedMesh ? 1 : nodes); ++index) {
                text += std::string(index == 0 ? "" : ",") + R"({"primitives": [{"attributes": {"POSITION": 0}}]})";
            }
            text += R"(], "nodes": [)";
            for (std::size_t index = 0; index < nodes; ++index) {
                text += std::string(index == 0 ? "" : ",") + R"({"name": ")" + std::string(name) + R"(", "mesh": )" +
                        std::to_string(sharedMesh ? 0 : index) +
                        (index + 1 < nodes ? R"(, "children": [)" + std::to_string(index + 1) + "]}" : "}");
            }
            return text + "]}";
        };
        const auto start = std::chrono::steady_clock::now();
        const ith::ImportOutput unnamed = importOk(chain(20000, ""));
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(30));
        REQUIRE(unnamed.objects.size() == 20000);
        CHECK(unnamed.objects.back().name.empty());
        // One unnamed mesh used by every node of the chain is named once, by the first node.
        const auto sharedStart = std::chrono::steady_clock::now();
        CHECK(importOk(chain(20000, "", true)).objects.size() == 1);
        CHECK(std::chrono::steady_clock::now() - sharedStart < std::chrono::seconds(30));

        const ith::ImportOutput shallow = importOk(chain(3, "N"));
        CHECK(namesOf(shallow) == std::vector<std::string>{"mesh N", "mesh N/N", "mesh N/N/N"});
        // Named chains grow their paths until they pass kGltfMaxNameSize.
        CHECK(errorCode(chain(20000, "Node")) == "import.too_large");
    }

    TEST_CASE("huge accessors fail before anything is allocated") {
        CHECK(ith::kGltfMinDecodeBudget <= ith::kGltfMaxDecodeBudget);
        // A zero-filled accessor asking for 2^36 bytes fails at once, whatever the file size.
        Value document = embeddedTriangle();
        Value& positions = at(document, "accessors", 0);
        positions.remove("bufferView");
        positions.remove("byteOffset");
        positions.set("count", Value::unsignedInteger(std::uint64_t{1} << 32U));
        const auto start = std::chrono::steady_clock::now();
        CHECK(errorCode(json(document)) == "import.too_large");
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
    }
}

TEST_SUITE("asset_importers.gltf_seeds") {
    // The fuzz seeds are valid files, so the fuzzer starts from inputs that import.
    TEST_CASE("the fuzz seeds import") {
        const std::filesystem::path seeds = "tests/asset_importers/fuzz_seeds/gltf";
        const ith::GltfImporter importer;
        for (const char* name : {"embedded.gltf", "triangle.glb"}) {
            CAPTURE(name);
            const auto file = ith::detail::asset_files::readFile(seeds / name, std::size_t{1} << 20);
            REQUIRE(file.hasValue());
            const ith::ImportOutput output = importOk(file.valueOr(std::string()));
            CHECK(namesOf(output) == std::vector<std::string>{"mesh Triangle", "mesh Root/Body", "material Steel"});
            CHECK(output.references == std::vector<std::string>{"Rock Color.png"});
        }
        const auto external = ith::detail::asset_files::readFile(seeds / "external.gltf", std::size_t{1} << 20);
        REQUIRE(external.hasValue());
        const std::string text = external.valueOr(std::string());
        CHECK(importer.dependencies(bytesOf(text), importer.defaultSettings()).valueOr(std::vector<std::string>{}) ==
              std::vector<std::string>{"triangle.bin"});
    }
}
