# Assets

Every asset has an id that survives renames, moves and reimports, and every reference between
assets is an id. The design is [ADR-0015](../decisions/0015-asset-identity-and-reimport.md); this
page shows how to use what exists today:

- `assets` (core, runtime): `AssetId`, `SubAssetId` and the reflected reference types;
- `asset_database` (editor layer, tools only): sidecars, native asset headers, the scan of a
  project's `Content` folder, move, rename and delete, importers with import and non-destructive
  reimport, and the local derived-data cache (DDC);
- `asset_importers` (editor layer, tools only): the glTF 2.0 importer and the payloads of
  imported meshes and materials.

The runtime asset manager, the cooked-data loader, the file watcher, the dependents index and
the image and audio importers come later (see [What is not there yet](#what-is-not-there-yet)).

## Ids and references

```cpp
#include "ith/assets/AssetRef.h"

struct Prop {
    ITH_TYPE(Component)
    ITH_FIELD()
    ith::AssetRef mesh;      // hard: loads with the prop
    ITH_FIELD()
    ith::SoftAssetRef next;  // soft: loaded on request
};

prop.mesh = ith::AssetRef::to(shipId, hullMeshId);
```

- `AssetId` and `SubAssetId` are random `ith::Uuid`s with distinct types. They are never derived
  from a path.
- A reference stores ids only, never a path, so it can't go stale on a rename. In JSON it is
  `{"asset": "<uuid>", "subAsset": "<uuid>"}`; a nil sub-asset means the asset itself and a nil
  asset means "no reference".
- Hard or soft is the field's type. `assetReferenceKind(type)` tells tools that follow references
  (the cooker, the dependents index) which one a reflected type is.

## Where the id lives

| Asset | Id stored in | Example |
|---|---|---|
| Level: an `.ithmap` document | its header | `{"format": "ith.level", "version": 1, "id": "...", "data": {...}}` |
| Other engine-native asset (prefab, material, data asset): an `.ithasset` document | its header | `{"format": "ith.material", "version": 2, "id": "...", "data": {...}}` |
| Imported source file (glTF, PNG, WAV, JSON, ...) | the sidecar `<file>.meta` next to it | `Ship.gltf` + `Ship.gltf.meta` |

Native extensions are matched without ASCII case
([ADR-0024](../decisions/0024-asset-document-envelope-and-extension.md)). A plain `.json` file in
`Content` is a source file like any other: it is ignored unless an importer reads it. A scan
reports an `.ithmap` file that isn't a level, or an `.ithasset` file that is one
(`FormatMismatch`); the asset stays in the database.

A sidecar always moves, copies and is committed with its file. It holds the id, the importer and
its version, the import settings, the sub-asset table and the engine-side overrides:

```json
{
  "format": "ith.asset_meta",
  "version": 1,
  "id": "3f2b8c1e-9d4a-4c1b-8e2f-0a1b2c3d4e5f",
  "data": {
    "importer": "gltf",
    "importerVersion": 1,
    "settings": {},
    "subAssets": [
      {
        "key": "mesh:Body/Hull",
        "id": "8a1d0f3e-5b6c-4d7e-9f80-1a2b3c4d5e6f",
        "missing": false
      }
    ],
    "overrides": {}
  }
}
```

- The header carries the asset id next to `format` and `version` (ADR-0015 rule 2, ADR-0024),
  around the `data` envelope of [ADR-0020](../decisions/0020-serialization-rules-completed.md). A
  header key given twice is refused.
- The sub-asset table is sorted by key and written with two-space indentation and LF line
  endings, so sidecars diff cleanly. Ids are never regenerated on save. A sidecar with CRLF line
  endings still reads, and is written back with LF when it is rewritten. Game projects should keep
  these files LF in git with `*.meta text eol=lf` (and the same for `*.ithasset` and `*.ithmap`)
  in their `.gitattributes`.
- Unknown keys are dropped with a warning (`ImportReport::warnings`); a version newer than the
  engine's is refused. Overrides are stored and written back unchanged; applying them is a later
  step.
- `readSidecar` and `writeSidecar` (`Sidecar.h`) read and write them; `readAssetHeader`
  (`AssetHeader.h`) reads a native asset's header without knowing its format.

## Source keys and reimport

An importer returns the objects of a source file with a type and, where the source names them, a
name path. Their **source keys** are:

| Object | Key |
|---|---|
| named | `mesh:Body/Hull` |
| a second object of the same type and name | `mesh:Body/Hull#1` (then `#2`, ...) |
| unnamed | `mesh:#0`: its index among the unnamed objects of its type |

`%` and `#` inside names are written `%25` and `%23`, so the forms can't collide.

Reimport matches sub-assets by key: matches keep their ids, new objects get new ids, and objects
that disappeared stay in the table marked `"missing": true`, so references to them can be
reported instead of silently breaking. If the object comes back under its old key, it gets its
old id back. Reordering objects with distinct names changes nothing. Objects without a name, and
objects that share a type and name, are told apart by their order among themselves, so reordering
those swaps their ids: give objects unique names in the DCC tool.

## The asset database

```cpp
#include "ith/asset_database/AssetDatabase.h"

ith::ImporterRegistry importers;
importers.add(std::make_unique<MyImporter>());

ith::AssetDatabase database(projectDirectory, importers); // assets in <project>/Content
const auto problems = database.scan();                    // rebuild from disk
const auto report = database.import("Meshes/Ship.gltf");  // import or reimport
const auto moved = database.move(report->id, "Vehicles/Ship.gltf");
```

- Paths are relative to `Content`, UTF-8, with `/` separators, and compared case sensitively.
  `isValidContentPath` refuses what some platform can't store: invalid UTF-8, `<>:"\|?*`, control
  characters, segments ending in `.` or space, segments over 255 bytes, Windows device names
  (`CON`, `PRN`, `AUX`, `NUL`, `COM1` to `COM9`, `LPT1` to `LPT9`, also with an extension), `..`,
  and names ending in `.meta` in any case.
- Only ASCII letters are compared without case. `É.png` and `é.png` are two paths to the database
  but one file on Windows and macOS; avoid names that differ only in non-ASCII case.
- Symbolic links: a link to a file is read as that file (a sidecar that is a link becomes a plain
  file when it is rewritten); links to folders are not followed; dangling links are left out.
- `folders()` lists every folder below `Content` (not `Content` itself), sorted, empty ones
  included and links to folders left out, as the last `scan()` found them; `move()` adds the
  folders it creates, and every asset added (`import()`, `assignNewId()`, `move()`) the folders
  along its path. The Content Browser's folder tree is built from it.
- `scan()` goes at most `kMaxFolderDepth` (64) folders deep below `Content`: a deeper folder is an
  `InvalidFile` problem, neither entered nor listed, so every walk of the folders is bounded.
- Files are read only up to a limit, checked before anything is allocated: 64 MiB for sidecars,
  256 MiB for native assets, 4 GiB for source files.
- `scan()` reports problems and never fails on them; only a `Content` folder that can't be listed
  fails it:

  | Problem | What happens |
  |---|---|
  | `DuplicateId` | Two files with one id, typically a copy with its sidecar. If the database knew the id at one of the paths, that file keeps it and the copy gets a new id, written to its sidecar or header. On a first scan nothing tells the copy from the original: the first path in byte order keeps the id in memory, nothing is written, and the other file stays out of the database until the user calls `assignNewId(path)`. |
  | `OrphanSidecar` | A sidecar without its file. It is left alone so it can be relinked. |
  | `MissingSidecar` | A file an importer reads, without a sidecar. It is not in the database until it is imported. |
  | `CaseConflict` | Two paths, or a sidecar and its file, that differ only in case (a `.META` suffix too): the project would break on another file system. |
  | `InvalidFile` | A sidecar or native asset that can't be read, a folder that can't be listed, a name that isn't valid Unicode, or a `<name>.tmp-<uuid>` file left by an interrupted write, with the reason. |
  | `FormatMismatch` | A native asset whose extension doesn't fit its format. |

  A scan doesn't create sidecars. A file whose sidecar was lost would otherwise get a new identity
  before the user could relink it (ADR-0015 rule 6); it gets one when it is imported.
- `move`, `rename` and `remove` move or delete the sidecar with the file and update the database.
  A move refuses a target where anything exists, also in another case, and a change of
  extension. Renaming a file to another case of its own name works. If a sidecar can't follow its
  file, the file is moved back; if even that fails, the error names both places and the record
  follows the file.
- `import` reads the source, takes the importer's output from the DDC when every input is
  unchanged, matches sub-assets and rewrites the sidecar only when it changed. A file moved
  outside the editor keeps its id when it is imported before a rescan; a copy whose original is
  still in place gets a new one. A sidecar in another case (`rock.png.meta` next to `Rock.png`) is
  a conflict, not something to overwrite. A failed import changes nothing.
- File access goes through the `file_system` module ([guide](file-system.md)). Sidecars, id
  rewrites and native assets are written with `fs::writeFileAtomic` and `Durability::Durable`: a
  reader never sees half a file, and the new content is flushed to the disk before the call
  returns. Paths are UTF-8 (`AssetDatabase` takes the project folder as a UTF-8 path).
- The database is single-threaded (the editor's main thread). It keeps no files open.

## Importers

```cpp
class MyImporter final : public ith::Importer {
public:
    std::string_view id() const override { return "my.format"; }   // stored in sidecars
    std::uint32_t version() const override { return 1; }           // bump when output changes
    std::span<const std::string_view> extensions() const override; // lowercase, with the dot
    ith::serialization::Value defaultSettings() const override;    // a JSON object
    ith::Expected<ith::ImportOutput> import(std::span<const std::byte> source,
                                            std::span<const ith::ImportFile> files,
                                            const ith::serialization::Value& settings) const override;
};
```

Importers must be deterministic, thread-safe and treat their input as untrusted; each one gets a
fuzz target. Extensions are matched without ASCII case (`Rock.PNG` uses the `.png` importer).

- **Other files an import reads** (a glTF's external buffers) are named by `dependencies()`, as
  paths relative to the source's folder. Importers never open files: the database reads them and
  passes them to `import()` in `files`, sorted by path. They must stay inside the source's folder
  (no `..`, absolute paths or drive letters), exist with exactly that case, and number at most
  `kMaxImportDependencies` (256); together with the source they are read up to the 4 GiB source
  limit. Their content is part of the DDC key, so changing a `.bin` reimports its `.gltf`. A
  missing one fails the import with `asset.not_found`.
- **Files the imported objects refer to** without the import reading them (a material's images)
  are returned in `ImportOutput::references`, relative to the source's folder.
  `ImportReport::references` lists them relative to `Content`. They are recorded, not checked:
  the image importer and the dependents index come later.

## The glTF importer

```cpp
#include "ith/asset_importers/GltfImporter.h"

importers.add(std::make_unique<ith::GltfImporter>()); // ".gltf" and ".glb", id "gltf"
```

It is our own reader on the serialization module's JSON parser; no glTF library is linked.

| Source | What happens |
|---|---|
| `.gltf` | JSON. Buffers are base64 `data:` URIs or files next to it. |
| `.glb` | Told from `.gltf` by its magic number. The header and chunk lengths must match the file, chunks are 4-byte aligned, the first chunk is JSON, an optional second is `BIN`, other chunk types are skipped. |
| buffer URIs | `data:<type>;base64,...` (canonical base64: padding optional, unused bits zero), or a relative path, percent-decoded first, that stays inside the source's folder. Other schemes, `..`, `.`, empty segments, absolute paths, drive letters, backslashes, queries and fragments are refused (`import.invalid_uri`). Image URIs follow the same rules. |
| accessors | Every component type, `normalized`, `byteStride` (4 to 252, a multiple of 4), sparse accessors and accessors without a buffer view (zeros). Offsets must be aligned to the component size; every element must lie inside its buffer view, which must lie inside its buffer, checked with overflow-safe arithmetic. `min` and `max` are ignored. |
| meshes | One sub-asset `mesh` per mesh, its payload an `ImportedMesh`. Each primitive must be a triangle list (`mode` 4; other modes fail with `import.unsupported`) with `POSITION` (VEC3 floats); `NORMAL` (VEC3 floats) and `TEXCOORD_0` (VEC2 floats or normalized unsigned bytes or shorts) are read when present, other attributes and morph targets are ignored. Indices are unsigned bytes, shorts or ints, a multiple of 3, each less than the vertex count; a primitive without them gets `0, 1, 2, ...`. Values must be finite. |
| materials | One sub-asset `material` per material, its payload an `ImportedMaterial`: base color, metallic and roughness factors, emissive factor, alpha mode and cutoff, double-sidedness, and the base color, metallic-roughness, normal (with its scale), occlusion (with its strength) and emissive textures with their texture coordinate set. A texture's image file is recorded as a reference; embedded images are not imported yet. |
| `extensionsRequired` | Any required extension fails the import with `import.unsupported`; optional extensions are ignored. |

**Source keys.** A mesh is named by its own name. A mesh without one is named by the name path of
the first node, in node order, that uses it: the names of its named ancestors and its own, joined
by `/`, so `mesh:Ship/Sail`. A material is named by its name. Unnamed objects fall back to their
index among the unnamed objects of their type, and repeated names get `#1`, `#2`, ... (see
[Source keys and reimport](#source-keys-and-reimport)). Reordering meshes, materials or nodes in a
re-exported file therefore keeps every id; renaming a node changes the key of an unnamed mesh it
names.

**Payloads.** `ImportedMesh` and `ImportedMaterial` (`ImportedModel.h`) are reflected types,
written with the binary archive under the formats `ith.imported_mesh` and `ith.imported_material`,
version 1:

```cpp
ith::ImportedMesh mesh;
const auto read = ith::serialization::readBinary(subAsset.payload, ith::kImportedMeshFormat, mesh);
```

A primitive holds its positions, normals and texture coordinates as flat float arrays, its
indices as `uint32` and its material as the material's source key (`"material:Steel"`, empty for
the default material). This is the importer's output, not the runtime mesh format, which cooking
produces later.

**Limits.** The JSON is read with the serialization module's limits (256 MiB, depth 64). Names
(of meshes, materials and nodes), node name paths and file URIs are at most 4 KiB
(`kGltfMaxNameSize`). What the import builds beyond the JSON counts against a budget: buffers
from `data:` URIs, accessor data, output arrays, and every copy of a name, path or key (a material
key in each primitive, an image path in each texture slot, node paths). The budget is 16 times the
size of the source and its buffers, at least 64 MiB and at most 4 GiB; more fails with
`import.too_large` before it is allocated, so a small file can't make the importer build gigabytes.
The payloads written from that data are about as large again. Primitives that share an accessor
decode it once; each still gets its own copy. Node paths are built once per node, so deep node
trees take linear time.

`dependencies()` and `import()` each parse the JSON: the database calls `dependencies()` on every
import to build the cache key, and `import()` only on a cache miss.

**Errors.** `import.malformed` names the JSON path of the problem, e.g.
`accessors[3]: its elements reach past the end of buffer view 1`; `import.unsupported`,
`import.invalid_uri` and `import.too_large` as above.

## Derived-data cache

- One file per entry in `<project>/Intermediate/DerivedData/`, which projects keep out of version
  control. Entries are binary archive blobs (ADR-0010) of at most 1 GiB; a damaged, outdated or
  oversized entry is a miss and is replaced.
- The key (`derivedDataKey`) hashes the source's content, every dependency's content hash, the
  importer id and version, the settings, the overrides, the platform (`"any"` for importer output)
  and the output format's version and schema hash, each length-prefixed.
- Hashes are XXH3-128 (`Hash128.h`). The function is private to `Hash128.cpp`.
- Writes are atomic but not flushed (`fs::Durability::Atomic`): a power loss can lose an entry,
  which is rebuilt, and imports don't pay a flush per entry. The cache is thread-safe. When an
  entry can't be replaced because another program has it open (Windows, `fs.busy`), the existing
  entry, made from the same inputs, is kept.

## Tests

- `assets`: ids, references, reference kinds and their JSON and binary round trips.
- `asset_database` (built with the tools, sanitizer builds included): sidecar round trip,
  versions and every error; native headers and extensions; source keys; import and reimport with a
  small text importer (reorder, rename, add, remove, unnamed objects, settings, importer versions,
  files moved or copied outside the editor, warnings, failures, files read besides the source and
  references); scan problems; move, rename, delete, links and UTF-8 paths; size limits; DDC keys,
  hits, misses, damaged and open entries; XXH3 known answers. POSIX-only cases cover unreadable folders and names that are not UTF-8, and
  a Windows-only case a sidecar that can't be moved. Each test uses its own temporary project
  folder and deletes it.
- `asset_sidecar_fuzz` (`tests/asset_database/SidecarFuzz.cpp`) fuzzes the sidecar and native
  header readers and `replaceHeaderId`, and checks that whatever reads writes back the same value.
  It runs in the Clang ASan build.
- `asset_importers` (built with the tools): the glTF importer on embedded, external and GLB
  buffers; interleaved, normalized, sparse and zero-filled accessors; generated indices; mesh and
  material names, node paths and duplicate names; reimport through the database with meshes,
  materials and nodes reordered and a node renamed; the DDC round trip and a changed external
  buffer; malformed containers, documents, bounds, strides, overflows, indices, sparse data,
  non-finite values, URIs that leave the folder and node cycles; the fuzz seeds.
- `gltf_import_fuzz` (`tests/asset_importers/GltfFuzz.cpp`) fuzzes `dependencies()` and
  `import()` with the input as every external buffer, and checks that whatever imports has valid
  keys, references and payloads whose indices are in range. Seeds are in
  `tests/asset_importers/fuzz_seeds/gltf`. It runs in the Clang ASan build.

## What is not there yet

- The editor file watcher, the persisted database cache and the dependents index (ADR-0015
  rules 5 and 7); relinking orphans by content hash.
- Applying overrides (rule 10), the cooker (rule 12) and the runtime asset manager.
- The image and audio importers; embedded glTF images, skins, animations, morph targets, more
  vertex attributes, line and point primitives, and glTF extensions (compressed geometry needs a
  decoder library and its own audit).
- One envelope for every reader: `readText` and `writeText` read and write the `id` of asset
  documents (the input action and context assets use it), but the sidecar and native header
  readers still parse their own envelope, and the envelope has no fuzz target of its own (ADR-0024).
