# ADR-0015: Asset identity and non-destructive reimport

- **Status:** Accepted
- **Amended by:** [ADR-0024](0024-asset-document-envelope-and-extension.md),
  [ADR-0033](0033-reflection-property-paths-and-patches.md)
- **Date:** 2026-11-20
- **Related:** [ADR-0008](0008-editor-familiar-to-unreal-developers.md) (Content Browser mirrors
  disk, no redirectors), [ADR-0010](0010-serialization.md) (JSON source, binary cooked data,
  migrations, derived-data cache keys), [ADR-0011](0011-shader-language-and-pipeline.md) (shader
  cache), [ADR-0014](0014-ecs-storage-and-world.md) (stable object ids, prefabs by copy and
  overrides)

## Context

Every asset reference in levels, prefabs, materials and code must survive renames, moves and
reimports. Requirements:

- The Content Browser mirrors the folders on disk and supports import, reimport, rename and
  move, with **no redirector files** (ADR-0008).
- **Reimport is non-destructive:** a changed source file (a glTF re-exported from a DCC tool)
  keeps its asset ids, the ids of the meshes, materials and animations inside it, its import
  settings and every engine-side change (material assignments, collision, LOD settings).
- Prefabs nest, and changes to a prefab reach instances that haven't overridden them
  (architecture). Their UX should feel familiar to Unreal users.
- Projects live in git: text where possible, stable output, binaries in Git LFS.
- Cooking produces per-platform binary data (ADR-0010), cached in a derived-data cache.

Lessons from earlier engine prototypes by the same maintainer:

- Random ids in a `.meta` sidecar next to each file, references by id only, and a dependents
  index (what uses this texture, before deleting it) worked.
- Deriving the id of a mesh inside a model from its **position** in the file broke references
  when the source was re-exported in a different order.
- A full rescan of the folder was the only way to see changes; files copied together with
  their `.meta` produced duplicate ids that had to be detected; paths had to be compared case
  sensitively so a project made on Windows worked on Linux.
- Prefab instances saved as full copies in the scene, with overrides recorded as property paths
  (`Light.intensity`, `+Light` for an added component, `-Light` for a removed one, markers for
  deleted entities), worked as a format, but every instance repeated the prefab's data in the
  scene file, and updating instances was an explicit sync step.
- Another prototype's design keyed the derived-data cache on source hash, importer version,
  settings and platform, and kept engine-side edits as overrides separate from imported data.

## Options considered

### Where the id lives

- **Path as identity** (no ids). Simple, but every move or rename breaks references or needs
  redirectors, which ADR-0008 rules out.
- **Ids derived from paths.** Same ids on every machine without any file, but they change on
  move. Kept only for built-in engine assets, whose ids are fixed constants.
- **A sidecar `.meta` for every asset.** One rule, but engine-native files (levels, prefabs,
  materials) can hold their own id, so the sidecar only doubles the files to move and commit.
- **The id inside engine-native files, a sidecar only for imported source files** (chosen).
  Native files are already JSON with a header (ADR-0010); a PNG or glTF can't hold our data.
- **One central database file** for the project. A merge-conflict magnet; rejected.

### Prefab instances in a level

- **Full copy plus override records.** Loads without the prefab, but repeats the prefab in every
  level, makes diffs noisy, and needs a sync step to propagate changes.
- **Reference plus override records** (chosen). Small, readable diffs; the prefab's current
  state is the base on every load, so changes propagate by construction.
- **Runtime inheritance in the ECS.** Ruled out by ADR-0014.

## Decision

### Identity

1. **Every asset has an `ith::Uuid`**, random, created when the asset is created or first
   imported, and never derived from its path. Built-in engine assets have fixed, documented ids.
2. **Engine-native assets** (levels, prefabs, materials, data assets) are JSON files whose header
   holds `format`, `version` and `id` (ADR-0010). **Imported source files** (glTF, PNG, WAV, …)
   get a text sidecar `<file>.meta` holding the id, the importer and its settings, the
   sub-asset table and the engine-side overrides. A sidecar always moves, copies and is
   committed with its file.
3. **Sub-assets** (meshes, materials, skeletons, animations inside one source file) have their
   own random ids, kept in the sidecar's sub-asset table with a **stable source key**: the
   object's type and name path in the source (for example `mesh:Body/Hull`), falling back to the
   index only for unnamed objects. Ids never come from position alone.
4. **References between assets are by id** (plus a sub-asset id where needed), never by path,
   and files store the id only, with no path hint that would go stale on a rename; the editor
   shows names.
   A reference is **hard** (loaded together with the referencing asset: a mesh's material) or
   **soft** (loaded on request, asynchronously: the next level, optional content). The kind is
   part of the reflected property.

### Asset database

5. **The editor and tools keep an asset database:** id ↔ path, type, sub-assets, dependencies
   and dependents. Assets live in the project's `Content/` folder, familiar to Unreal users. The
   database is built by a scan of that folder at start-up, kept in a local cache that is never
   committed, and updated three ways:
   - **Editor operations** (create, rename, move, duplicate, delete in the Content Browser) move
     files with their sidecars and update the database directly.
   - **A file watcher** in the editor picks up outside changes, debounced. A rename done outside
     the editor keeps its identity when the sidecar moved with it (or, for a native asset, from
     the id in the file).
   - **A rescan** on request rebuilds it from disk.
6. **Problems are reported, not fatal:** a duplicate id (a file copied with its sidecar) gives
   the copy a new id; a sidecar without its file, or a file without its sidecar, is listed so
   the user can relink it (matched by content hash where possible). Paths are compared case
   sensitively, and two paths that differ only in case are an error, so a project behaves the
   same on Windows and Linux.
7. **Deleting** an asset in the Content Browser first shows its dependents and offers to replace
   their references.

### Import and reimport

8. **Initial importers:** glTF 2.0 (`.gltf`, `.glb`) for models, skeletons and animations; PNG,
   JPEG, TGA and HDR images; WAV, FLAC and Ogg Vorbis audio, all in P1. FBX follows right after
   through the same importer interface. Importers are tools-only and every one has a fuzz target.
9. **Reimport is non-destructive.** It reruns the importer with the saved settings, then matches
   sub-assets by source key: matches keep their ids, new objects get new ids, and objects that
   disappeared stay in the table marked missing, so references to them are reported (with a
   remap UI) rather than silently broken.
10. **Engine-side edits to imported assets are overrides** in the sidecar, keyed by sub-asset id
    and property path, applied after import: imported data, then engine defaults, then
    overrides. An override that no longer resolves is reported; following ADR-0010, the editor
    warns and drops it on the next save, and cooking fails on it.

### Derived data and cooking

11. **Derived-data cache (DDC):** the key is a content hash of at least 128 bits (the function
    is chosen with the DDC implementation) over the source and every dependency, the importer
    id and version, the settings and overrides, the target platform and the output format
    version. Entries use the binary archive of ADR-0010 and live
    in a local, per-project cache that is never committed. The shader cache uses the same DDC
    (ADR-0011). A shared DDC comes later, behind the same interface, when builds or the team
    need it.
12. **The cooker** starts from the project's start levels and its "always cook" list, follows
    hard and soft references, and writes each reachable asset for one platform from the DDC.
    Unreferenced assets are not cooked. A missing reference or an unknown field is a cook error.

### Prefabs

13. **A prefab is an engine-native asset**: a group of entities with one root, each with an id
    local to the prefab.
14. **An instance in a level** is stored as the prefab id, the instance root's own transform and
    a list of **override records**, addressed by the entity's id in the prefab:
    - `"<entity>:Light.intensity"`: a property with its own value (the value is stored);
    - `"<entity>:+Light"` / `"<entity>:-Light"`: a component added or removed;
    - added entities (stored in full) and removed prefab entities (by id).

    Loading builds the instance from the prefab's current state, then applies the overrides, so
    every change to the prefab reaches every value that isn't overridden. Each entity of the
    instance gets a level-unique stable object id derived from the instance and the prefab
    entity ids.
15. **Nested prefabs:** a prefab may contain instances of other prefabs, with their overrides;
    override addresses chain entity ids through the nesting. A prefab that contains itself,
    directly or indirectly, is refused.
16. **Editor UX**, familiar from Unreal: overridden properties are marked in Details, each with a
    reset arrow that reverts it to the prefab's value; the instance has **Apply** (push its
    overrides into the prefab asset, all or per property) and **Revert all**; **Unpack** turns an
    instance into plain entities. Opening a prefab edits it in its own editor tab; saving it
    updates open levels at once.
17. **A missing prefab** leaves a placeholder entity with the overrides kept and an error in the
    log; saving keeps the record so nothing is lost.

### Levels and version control

18. **A level is an engine-native asset**: its entities, in hierarchy order, each with its
    stable object id (ADR-0014). Large worlds are split into cells by the open-world ADR.
19. **Version control friendliness:** one asset per file; text JSON for native assets and
    sidecars with stable key and entity order (ADR-0010); ids are never regenerated on save; no
    timestamps, user names or absolute paths in files; LF line endings. Binary sources (images,
    meshes, audio, `.blend`) go to Git LFS through `.gitattributes`. A file containing merge
    conflict markers fails to load with an error naming the file.

### Broken references

20. **A reference that doesn't resolve** loads as a typed placeholder (a checker material, a
    default mesh, silence), is logged once with the referencing asset and property, and is
    listed in the editor's broken-reference view. Games built for shipping never get there,
    because cooking fails on it (rule 12).

### Placement

21. Ids, references, the runtime asset manager (asynchronous loading by generational handle) and
    the cooked-data loader are in `core/assets`. The asset database, importers, sidecars and the
    cooker are tools-only modules that the player never links.

## Consequences

- Moves, renames and reimports never break references, and no redirector files exist.
- Imported files carry a sidecar that must be committed with them; the editor moves them
  together, and the problem list catches what happens outside it.
- Levels hold prefab references, not copies, so a level can't be loaded meaningfully without
  its prefabs; the placeholder rule keeps the data safe.
- Sub-assets need names in the source for the best reimport results; unnamed objects fall back to
  their index.
- Follow-up work: the asset database and watcher; sidecar format; the glTF and image importers
  with fuzz targets; the DDC; prefab instancing, overrides and the Details markings; the cooker.
- **FBX import gets a dependency audit first.** The candidate is the MIT-licensed ufbx; the
  proprietary Autodesk FBX SDK is not used.
- **The DDC's 128-bit content hash gets a dependency audit** when the DDC is built (for example
  xxHash's XXH3-128).
- **Revisit when:**
  - the team grows or builds slow down (shared DDC);
  - level files become merge-conflict hot spots (split levels into per-entity files with the
    open-world ADR);
  - a source format needs data we can't key stably.
