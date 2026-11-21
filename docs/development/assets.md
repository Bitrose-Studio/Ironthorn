# Assets

Every asset has an id that survives renames, moves and reimports, and every reference between
assets is an id. The design is [ADR-0015](../decisions/0015-asset-identity-and-reimport.md); this
page shows how to use what exists today:

- `assets` (core, runtime): `AssetId`, `SubAssetId` and the reflected reference types;
- `asset_database` (editor layer, tools only): sidecars, native asset headers, the scan of a
  project's `Content` folder, move, rename and delete, importers with import and non-destructive
  reimport, and the local derived-data cache (DDC).

The runtime asset manager, the cooked-data loader, the file watcher, the dependents index and
the real importers come later (see [What is not there yet](#what-is-not-there-yet)).

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
- Writes go to a temporary file that is renamed over the target, so a reader never sees half a
  file. They are not flushed to the disk, so a power loss can lose the latest write.
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
                                            const ith::serialization::Value& settings) const override;
};
```

Importers must be deterministic, thread-safe and treat their input as untrusted; each one gets a
fuzz target. Extensions are matched without ASCII case (`Rock.PNG` uses the `.png` importer).

## Derived-data cache

- One file per entry in `<project>/Intermediate/DerivedData/`, which projects keep out of version
  control. Entries are binary archive blobs (ADR-0010) of at most 1 GiB; a damaged, outdated or
  oversized entry is a miss and is replaced.
- The key (`derivedDataKey`) hashes the source's content, every dependency's content hash, the
  importer id and version, the settings, the overrides, the platform (`"any"` for importer output)
  and the output format's version and schema hash, each length-prefixed.
- Hashes are XXH3-128 (`Hash128.h`). The function is private to `Hash128.cpp`.
- Writes are atomic (a temporary file renamed over the entry), so the cache is thread-safe. When
  an entry can't be replaced because someone has it open (Windows), the existing entry, made from
  the same inputs, is kept.

## Tests

- `assets`: ids, references, reference kinds and their JSON and binary round trips.
- `asset_database` (built with the tools, sanitizer builds included): sidecar round trip,
  versions and every error; native headers and extensions; source keys; import and reimport with a
  small text importer (reorder, rename, add, remove, unnamed objects, settings, importer versions,
  files moved or copied outside the editor, warnings, failures); scan problems; move, rename,
  delete, links and UTF-8 paths; size limits; DDC keys, hits, misses, damaged and open entries;
  XXH3 known answers. POSIX-only cases cover unreadable folders and names that are not UTF-8, and
  a Windows-only case a sidecar that can't be moved. Each test uses its own temporary project
  folder and deletes it.
- `asset_sidecar_fuzz` (`tests/asset_database/SidecarFuzz.cpp`) fuzzes the sidecar and native
  header readers and `replaceHeaderId`, and checks that whatever reads writes back the same value.
  It runs in the Clang ASan build.

## What is not there yet

- The editor file watcher, the persisted database cache and the dependents index (ADR-0015
  rules 5 and 7); relinking orphans by content hash.
- Applying overrides (rule 10), the cooker (rule 12) and the runtime asset manager.
- Real importers: glTF (an own reader on the serialization module's JSON), images, audio.
- Writing native assets: the `id` header in the serialization module's `readText` and `writeText`,
  and the envelope and migration helper that sidecars and native assets share, come with the first
  native format (ADR-0024).
- Durable writes (flushing files and folders to the disk) need a file-system module in the
  platform layer; the `platform` module is a client module, which tools modules can't use.
