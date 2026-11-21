# ADR-0024: Asset documents: the envelope id and native asset extensions

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** amends [ADR-0020](0020-serialization-rules-completed.md) rule 1 (text document
  layout) and [ADR-0015](0015-asset-identity-and-reimport.md) rule 2 (native assets);
  [ADR-0008](0008-editor-familiar-to-unreal-developers.md) (Content Browser),
  [ADR-0010](0010-serialization.md) (serialization), [assets](../development/assets.md)

## Context

The first asset database slice exposed two gaps between accepted ADRs.

**The envelope has no place for the id.** ADR-0015 rule 2 puts `format`, `version` and `id` in
the header of an engine-native asset. ADR-0020 rule 1 defines one envelope for every text
document, `{"format", "version", "data"}`, and `readText` refuses anything else at the top level
when unknown fields fail. Native assets therefore can't be read or written through `readText`
and `writeText`, and the sidecar reader already re-implements envelope parsing and the
migration runner to get at its `id`. Every further native format would copy that code again.

**`.json` is not ours to claim.** The slice treats every `.json` file under `Content/` as a
native asset. Ordinary JSON files that a project keeps as content (Tiled maps, localisation
tables, Lottie animations, third-party data) then show up as `InvalidFile`, and no importer can
ever handle `.json`, because the database has already claimed the extension.

Prior art: Unreal stores every asset in one engine-specific container extension, `.uasset`, with
levels in `.umap`; the Content Browser learns an asset's type from the file, not from its
extension, and files are meant to be managed through the editor
([Working with assets](https://dev.epicgames.com/documentation/unreal-engine/working-with-assets-in-unreal-engine),
[Assets and packages, 4.27](https://docs.unrealengine.com/4.27/Basics/AssetsAndPackages)).
Unity uses per-type extensions (`.prefab`, `.mat`, `.unity`, `.asset`). Our files differ from
both in being text that people diff and occasionally edit by hand (ADR-0015 rule 19).

## Options considered

### The id in the envelope

- **An optional `id` in the shared envelope** (chosen). `{"format", "version", "id", "data"}`,
  where `id` is present for asset documents and absent for everything else. Migrations still see
  only `data`, so ADR-0020's protection of the header stands. One reader and one migration
  runner serve native assets, sidecars and plain documents.
- **The id inside `data`.** No envelope change, but the asset database would have to know every
  format's schema to find the id, and a migration could move or drop it. Rejected.
- **A second, asset-only envelope in the asset layer.** Keeps serialization unchanged, but is the
  duplication the slice already shows: two envelope parsers and two migration runners that must
  agree.

### The native asset extension

- **`.json`, reserved for native assets** (the slice). JSON tooling works everywhere, but
  ordinary JSON content is misreported and can never be imported. Rejected.
- **Per-type extensions** (`.level`, `.prefab`, `.material`, …). The type is visible in a file
  manager, but every new asset type, including game-defined data assets, needs a new extension,
  each one a fresh chance to disagree with `format`; and short generic names collide with other
  tools (`.prefab` and `.mat` are Unity's, `.material` and `.level` are common words). Rejected.
- **A compound suffix ending in `.json`** (`Ship.prefab.json` or `Ship.ith.json`). Editors
  highlight it as JSON with no setup. But every `.json` importer and the scanner must first
  exclude the compound form, and a rename in a file manager that drops the inner part silently
  turns an asset into plain JSON. Rejected.
- **One dedicated extension for every native asset, `.ithasset`.** The scanner's rule is one
  suffix, the type comes from the header's `format`, and `.json` is free for importers. But
  levels, the files people open, search for and review most, look like any other asset in a file
  manager or a diff, and can't get their own double-click association.
- **`.ithasset` for native assets and `.ithmap` for levels** (chosen), as Unreal separates
  `.uasset` and `.umap`. It keeps what the single extension gives, with `.json` free for
  importers, while levels stay recognisable outside the editor and can later get their own
  operating-system association. The price is one fixed pairing to check (`.ithmap` holds the
  level format and nothing else holds it), not one per type. Editors don't highlight either as
  JSON until told to, which is one line per tool (below).

## Decision

### Envelope

1. **Text document layout** (changes ADR-0020 rule 1):
   `{"format": <name>, "version": <n>, "id": <uuid>, "data": <value>}`, in that key order.
   `id` is present in **asset documents** (native assets and sidecars) and absent in all others.
   Migrations still operate on `data` only and can't change `format`, `version` or `id`.
2. **`serialization` owns the envelope**, with the `id` as an `ith::Uuid` (foundation), so it
   doesn't depend on `assets`:
   - a public envelope reader returns `format`, `version`, the optional `id` and the `data`
     tree, with the size, depth and allocation limits of every read;
   - a public migration function runs a `DocumentFormat`'s chain on that tree;
   - `readText` and `writeText` are built on them and accept an id to write and report the id
     read.

   The sidecar reader and the native header reader use these, and no other code parses an
   envelope.
3. **Whether an id is required is the caller's choice** for each read: a reader that expects an
   id refuses a document without one, or with a nil one; a reader that doesn't expect one
   refuses a document that has one. Both are `serialization.not_a_document`, an error regardless
   of the unknown-field mode, because a header mismatch is never stale data.
4. **This lands with the first native format**, which is also the first user of `writeText` for
   an asset. The binary header (ADR-0020 rule 3) is unchanged; how cooked data names its asset
   is decided with the cooker and the runtime asset manager.

### Extension

5. **Levels use the extension `.ithmap`; every other engine-native asset uses `.ithasset`**
   (changes ADR-0015 rule 2, which called them JSON files without naming an extension). The
   content of both is the JSON document of rule 1; the type is its `format`. Extensions are
   written lowercase and matched without ASCII case, like importer extensions.
6. **The extension and `format` must agree:** an `.ithmap` file holds the level format, and the
   level format is only in `.ithmap` files. A mismatch is a problem reported by the scan, naming
   the file, the expected and the found format, like the other scan problems (ADR-0015 rule 6).
   It is not fatal; the asset is not loaded until it is fixed.
7. **Only `.ithasset` and `.ithmap` files are read as native assets.** Every other file under
   `Content/` is a source file for an importer, `.json` included; without an importer it is
   ignored, as any other unknown extension is.
8. **Tooling:** the project template ships a `.gitattributes` with
   `*.ithasset text eol=lf linguist-language=JSON` and the same line for `*.ithmap` (GitHub
   shows them as JSON, per the
   [Linguist overrides](https://github.com/github-linguist/linguist/blob/main/docs/overrides.md)),
   and editor settings that map both extensions to JSON (for example VS Code's
   `files.associations`).

## Consequences

- Native assets and sidecars read, migrate and write through one code path; the asset database
  loses its private envelope and migration code.
- Plain JSON content can live in `Content/` and get an importer like any other source format.
- Files aren't recognised as JSON by a fresh editor until the project's settings are in place;
  the content is still plain JSON text.
- Levels are recognisable in file managers and diffs, and can get their own double-click
  association when the editor installs one.
- Changing an extension later means renaming every native asset in every project, so the choice
  is effectively permanent.
- Follow-up work: the envelope reader, migration function and id support in `readText` and
  `writeText` with tests (id required, refused, nil, key order, migrations not seeing it) and a
  fuzz target for the envelope reader; `kNativeAssetExtension` replaced by `.ithasset` and
  `.ithmap`, with the extension/format mismatch as a scan problem and its tests; the sidecar and
  header readers moved onto the shared envelope; the project template's `.gitattributes` and
  editor settings; `docs/development/assets.md` updated.
- **Revisit when:** a native asset needs a form other than one JSON document (a bulk binary
  part, a per-entity split for open worlds), which may need a container or folder format of its
  own.
