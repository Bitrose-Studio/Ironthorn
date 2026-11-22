# ADR-0035: File system, layered configuration and the main-thread queue

- **Status:** Accepted
- **Amended by:** [ADR-0036](0036-file-system-windows-atomic-replace-corrected.md)
- **Date:** 2026-11-22
- **Related:** amends [ADR-0025](0025-job-system.md) rule 7 (who drains the main-thread queue,
  and the wake-up), [ADR-0029](0029-editor-ui-toolkit.md) rules 12 (idle wake-up) and 15 (where
  layouts live) and [ADR-0027](0027-input-rules-completed.md) with the
  [input-actions spec](../specs/input-actions.md) (the bindings file location, open question 6);
  [ADR-0001](0001-cpp-language-and-safety-rules.md) (no exceptions, `ith::Expected`),
  [ADR-0004](0004-modules-and-layering.md) (layers), [ADR-0008](0008-editor-familiar-to-unreal-developers.md)
  rule 7 (layered configuration), [ADR-0010](0010-serialization.md) and
  [ADR-0020](0020-serialization-rules-completed.md) (envelope, missing fields take defaults,
  comments only in config), [ADR-0012](0012-windowing-and-event-loop.md) (main thread, `platform`),
  [ADR-0015](0015-asset-identity-and-reimport.md) (case rules, sidecars),
  [ADR-0017](0017-runtime-and-editor-separation.md) (module kinds),
  [ADR-0034](0034-level-documents.md) (the caller does level file I/O)

## Context

The editor must not lose a level to a crash, needs one place for engine, project, platform and
user configuration (layouts, keymap, preferences), and must get background results back on the
main thread every frame, also while it idles. Today:

- File code exists only inside `asset_database` (tools kind). Its atomic write renames a
  temporary file but never flushes it, so a power loss can lose the latest write. It uses
  `std::filesystem`, where a `char` path means the ANSI code page on Windows, and `std::ifstream`,
  which on Windows opens files without delete sharing, so our own reads can make a replacement
  fail.
- `platform` is `client` kind and doesn't depend on `jobs`. A dedicated server and the cooker need
  files and configuration, so neither can live there.
- `jobs` has `postToMainThread` and `runMainThreadJobs`, but nothing calls the latter. ADR-0029's
  idle policy waits for events with a timeout; a posted job or progress update must wake that
  wait.
- `serialization` already has `Value`, `readDocument`, `writeDocument`, `fromValue`, `toValue` and
  a JSON syntax with comments for config files. `input_actions` already merges its own four
  layers and writes only what differs from the layers below.

Prior art (public documentation and observed behaviour):

- **Unreal** layers configuration per category: engine `Base` files, engine platform files,
  project `Config/Default*.ini`, project `Config/<Platform>/`, then user files, with array
  operators (`+`, `-`, `.`, `!`) that don't work from the command line
  ([Configuration Files](https://dev.epicgames.com/documentation/en-us/unreal-engine/configuration-files-in-unreal-engine)).
  Editor settings, layouts and key bindings are categories of their own; the editor writes user
  changes into the project's `Saved` folder. A strict layer order and per-platform folders work;
  the array operators are a syntax of their own to learn.
- **Godot** keeps project settings in a committed project file, and editor settings, shortcuts
  and layouts in the user's configuration folder (`%APPDATA%\Godot`, `~/.config/godot`, XDG),
  shared by all projects; per-project editor state lives in the project
  ([Data paths](https://docs.godotengine.org/en/stable/tutorials/io/data_paths.html)).
- **Bevy** has no built-in layered configuration; settings are resources.
- **JSON Merge Patch** ([RFC 7396](https://www.rfc-editor.org/rfc/rfc7396)): objects merge by key,
  anything else replaces, `null` removes.

## Options considered

File system:

- **In `platform`.** It is `client` kind, so servers and the cooker couldn't use it. Rejected.
- **`std::filesystem` behind helpers**, as today. No file or directory flush, no control of
  Windows share modes, and the `char` path conversion trap. Rejected.
- **A `file_system` module on the OS APIs** (Win32 wide functions, POSIX) (chosen): small, a few
  hundred lines per OS, and every target can link it.

Configuration format and merge:

- **INI with array operators**, as Unreal. A second syntax beside ADR-0010's JSON, and operator
  semantics to learn. Rejected.
- **TOML.** A new dependency and a second parser. Rejected.
- **JSON in the ADR-0010 envelope, merged by RFC 7396** (chosen). One parser, a documented merge
  with three rules. It can't set a value to `null`, and lists edited item by item must be objects
  keyed by id.

Defaults:

- **In shipped engine files only.** They drift from the code that reads them. Rejected for
  settings.
- **In reflected struct initializers** (chosen): one source of truth. Engine files ship only data
  that is not a settings struct (the default layout, the keymap preset).

User layer of the editor:

- **Per project, in `<project>/Saved/`**, as Unreal. Keys must be rebound in every project.
  Rejected.
- **Per user, shared by all projects** (chosen, the owner's decision), as Godot and most IDEs;
  per-project session state stays in the project.

## Decision

### The `file_system` module

1. **`file_system`**, layer `platform`, kind `runtime`, depends on `foundation` only.
   - **Paths are UTF-8 `std::string_view` with `/` separators.** The module converts them to UTF-16
     for the wide functions on Windows and passes bytes elsewhere. `std::filesystem` is not in its
     API.
   - On Windows an absolute path gets the `\\?\` prefix after normalization, so long paths work
     without the manifest opt-in.
   - Path helpers, pure and lexical: `normalizePath` (`\` to `/`, repeated `/` and `.` collapsed,
     `..` resolved but never above the root, drive and UNC roots kept), `joinPath`, `parentPath`,
     `fileName`, `extension`.
2. **`readFile(path, maxSize) -> Expected<std::string>`** checks the size before it allocates
   (following symbolic links), and fails with `fs.too_large` above `maxSize`. On Windows it opens
   with read, write and delete sharing, so a reader never blocks an atomic replacement.
3. **`writeFileAtomic(path, bytes, options)`**: readers see the old file or the new one, never a
   partial one.
   - The bytes go to `<name>.tmp-<uuid>` in the target's folder; `isTemporaryFileName()` is public
     so scans report leftovers. Missing parents are created unless `createParents` is false.
   - **POSIX:** create with `O_CREAT | O_EXCL | O_CLOEXEC`, write in a loop (retrying `EINTR`),
     `fsync`, keep the existing target's permission bits, `rename`, then open the parent with
     `O_DIRECTORY` and `fsync` it. When parents were created, each new directory's parent is
     flushed too.
   - **Windows:** `CreateFileW` with `CREATE_NEW`, write, `FlushFileBuffers`, then `MoveFileExW`
     with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`. Windows documents no directory
     flush, and documents write-through explicitly only for moves done as copy and delete; **we
     claim no more than these calls give.** `ReplaceFileW` is not used: it fails when the target
     is missing, has partial-failure states, and merges attributes and ACLs we don't need.
   - A sharing violation or access denied on the replacement (virus scanners, the search indexer)
     is retried with a short backoff, about 200 ms in total, then fails with `fs.busy`.
   - `Durability::Durable` (the default) does the flushes above; `Durability::Atomic` skips them,
     for data that can be rebuilt.
   - The temporary file is removed on every failure.
4. **Other operations:**
   - `fileInfo(path) -> Expected<std::optional<FileInfo>>`: kind, size and modification time;
     `nullopt` when nothing is there.
   - `listDirectory(path) -> Expected<std::vector<DirectoryEntry>>`, one level: name, kind (file,
     directory, link to a file, link to a directory, other), size, and `validName`. A name that is
     not valid Unicode comes with a lossy display name, so callers can report it.
   - `createDirectories(path)`; `remove(path)` for a file or an empty directory.
   - `move(from, to)` **never replaces** an existing target: `MoveFileExW` without
     `REPLACE_EXISTING` on Windows; `renameat2` with `RENAME_NOREPLACE` on Linux, falling back to
     check-then-rename where the file system refuses the flag. A rename that changes only case is
     allowed when source and target are the same file (same volume and file id, same device and
     inode).
5. **Case.** The module never folds case. ADR-0015's case rules (exact-case existence, case
   variants) stay in `asset_database`, built on `listDirectory`.
6. **Known folders**, absolute and normalized:
   - `executableDirectory()`: `GetModuleFileNameW`, `/proc/self/exe`;
   - `userConfigDirectory()`: the roaming application data folder, `$XDG_CONFIG_HOME` else
     `~/.config`;
   - `userCacheDirectory()`: the local application data folder, `$XDG_CACHE_HOME` else
     `~/.cache`.

   Callers append `<Organization>/<Application>`. A user-data folder for save games comes with
   the save system. A console port implements the same functions or fails with `fs.unsupported`.
7. **Error codes:** `fs.not_found`, `fs.already_exists`, `fs.access_denied`, `fs.busy`,
   `fs.not_a_directory`, `fs.is_a_directory`, `fs.not_empty`, `fs.invalid_path`, `fs.too_large`,
   `fs.no_space`, `fs.unsupported`, and `fs.io` for the rest. The message names the path and
   gives the operating system's text.
8. **Threading.** Every function is thread-safe and blocks. Large reads and writes (level saves)
   run on the background lane and return through the main-thread queue.
9. **No file watcher yet.** P2 uses a Rescan command; the watcher joins this module after P2.
10. **One place for file I/O.** A lint test allows `<filesystem>`, `<fstream>`, `fopen` and
    `CreateFileW` under `engine/ith` only in `file_system`.

### The `config` module

11. **`config`**, layer `core`, kind `runtime`, depends on `foundation`, `file_system`,
    `serialization` and `reflection`.
12. **Layers.** A `Config` is built from an ordered list of layer directories, lowest first, each
    with a name and a writable flag; the application supplies the list.

    | # | Layer | Editor directories | Written |
    |---|---|---|---|
    | 1 | engine | `<exe>/Config/`, then `<exe>/Config/<Platform>/` | never |
    | 2 | project | `<project>/Config/` (committed) | only on explicit request |
    | 3 | platform | `<project>/Config/<Platform>/` (`Windows`, `Linux`) | only on explicit request |
    | 4 | user | `%APPDATA%\Ironthorn\Editor`, `~/.config/Ironthorn/Editor` (through `userConfigDirectory()`) | by default |

    - The build copies `engine/config/` to `<exe>/Config/`, as it copies the editor's fonts.
    - The **editor's user layer is per user and shared by all projects.**
    - A **player** reads layers 1 to 3 from its installation (the cooker decides how) and a user
      layer at `<userConfig>/<Organization>/<Game>/`. A **dedicated server** has layers 1 to 3.
    - Tests and automation pass explicit directories (the editor's `--user-dir <dir>`), so they
      never read or write a developer's real user layer.
    - **No command-line layer in P2.**
13. **Per-project session state is not configuration.** The last session's layout, and later
    recent levels and open tabs, are written by the editor to `<project>/Saved/Editor/`, outside
    the layers. Project templates ignore `Saved/` and `Intermediate/`.
14. **Format.** A settings file is a document in the ADR-0010 envelope, `format` `ith.config`,
    `version` 1, whose `data` is an object of named sections. Comments are allowed; files the
    engine writes have none. At most 1 MiB. A file that fails to parse or has a duplicate key is
    skipped with one warning: that layer contributes nothing for the file, and start-up goes on.
15. **Merge: RFC 7396, per file, from the lowest layer to the highest.**
    - An object over an object merges key by key, recursively.
    - Any other value, **arrays included**, replaces the value below it whole.
    - `null` removes the key.
    - There are no array operators. Data that layers edit item by item is an object keyed by a
      stable id, never an array: the keymap maps a command id to its chords, and `[]` unbinds.
16. **Typed access.** `ConfigSection<T>{file, section}` names a reflected struct, for example
    `kEditorPreferences{"Editor", "preferences"}`. `config.get(section) -> T` reads the merged
    section with `fromValue`. Missing fields keep the struct's defaults, which are the engine
    defaults. An unknown field warns once. A field of the wrong type warns and keeps its default.
17. **Writes.** `config.set(section, value, target = User)` writes into the target layer only the
    fields that differ from the merge of the layers below it, as a merge patch, and removes from
    it the fields that now equal them, so a later change of a lower default still reaches the
    user. The file is written at once with `writeFileAtomic`, durable, keeping existing key
    order and appending new keys, so diffs stay stable. On failure memory is unchanged. Writing
    to a layer that isn't writable is an assert. UIs commit a preference when the control is
    released, not on every frame of a drag.
18. **Whole files per layer**, for documents that are not merged: `findFile(relative)` returns
    the highest layer that has the file, `listFiles(folder)` the union by name with the highest
    layer winning and its origin, `writeFile(layer, relative, bytes)` writes one durably.
19. **Change notification.** `generation()` and `generation(file)` change on every `set`,
    `writeFile` and `reload()`. Consumers poll once per frame and read again; there are no
    callbacks. `reload()` reads every file again and backs a Reload command until the watcher
    exists.
20. **Threading.** A `Config` is not thread-safe. Its owner on the main thread hands copies of
    sections to jobs.
21. **Contents in P2** (this amends ADR-0029 rule 15 and settles the input-actions spec's open
    question 6):
    - `Editor.json`: `preferences` (idle rate 10 frames per second, 2 without focus) and `layouts`
      (the name of the chosen default layout);
    - `Keymap.json`: command id to chords; the engine layer ships the ADR-0008 preset;
    - `Layouts/<name>.json`: `ith.editor_layout` documents through rule 18; the engine layer ships
      `Layouts/Default.json`;
    - `Input.json`: the `ith.input_bindings` document of each layer, through rule 18. The
      application reads each layer's text and passes it to `InputBindingConfig::setLayer`, so
      `input_actions` keeps its own merge and doesn't depend on `config`.

### The main-thread queue

22. **Frame order** (amends ADR-0025 rule 7): the application's loop calls `pumpEvents()` (or
    `waitEvents(timeout)` while idle), then `jobs.runMainThreadJobs()`, then updates and renders.
    `platform` doesn't drain the queue: it is `client` kind, servers and tools have no
    `platform`, and it doesn't depend on `jobs`. The samples and the editor follow this order; a
    server calls `runMainThreadJobs()` at the start of its tick.
23. **Wake-up** (amends ADR-0025 rule 7 and ADR-0029 rule 12).
    `JobSystem::setMainThreadWake(void (*)(void*), void*)` registers a hook called after every
    main-thread post and progress update, from any thread; it must not block.
    `Platform::wake()` is thread-safe and pushes an internal event that ends `waitEvents` early and
    that `pumpEvents` drops. The application connects the two.
24. **No time budget** per frame for main-thread jobs until profiles show a need.

### Migration of `asset_database`

25. `FileIo.h` and `FileIo.cpp` are removed; the module uses `file_system`:
    - sidecars, id rewrites and native assets with `Durability::Durable`;
    - derived-data cache entries with `Durability::Atomic`, keeping today's rule that a busy entry
      which exists counts as written (`fs.busy`);
    - scan, case checks, move, rename and delete through `listDirectory`, `move` and `remove`;
      leftover temporary files through `isTemporaryFileName`.

    `AssetDatabase` and `DerivedDataCache` take UTF-8 paths. Their public error codes
    `asset.io` and `asset.too_large` stay, with the `fs` message inside.

### Tests

26. - Atomic write: a test-only, thread-local fault point (not in Shipping) fails after the
      temporary write, after the flush and before the rename; the old content stays whole and no
      temporary file remains. A busy target on Windows; permission bits kept on Linux.
    - `move`: a case-only rename, an existing target refused. Long paths and non-ASCII names on
      Windows; names that are not valid Unicode in listings on Linux. Known folders with and
      without the XDG variables.
    - Merge: objects, array replacement, `null` removal, a skipped broken file, defaults for
      missing fields, unknown-field warnings, `set` writing only the difference.
    - Wake-up: a post from a background job ends `waitEvents(1 s)` within 50 ms on the offscreen
      driver.
    - The existing `asset_database` suites pass unchanged.
    - Durability across a real power loss is not tested in CI.

## Consequences

- One file API for every target, correct UTF-8 paths on Windows, and durable saves before the
  first level save. Settings defaults live in one place, the struct.
- RFC 7396 can't set a value to `null`, and lists that layers edit item by item must be objects.
- Writing a commented project file drops its comments. The editor writes the project layer only
  on request.
- Cache entries can be lost on a power cut and are rebuilt; imports don't pay a flush per entry.
- Follow-up work: `Platform::waitEvents` and `Platform::wake`; the editor's `--user-dir`; copying
  `engine/config/` next to the executable; `.gitignore` in the project template; the watcher
  after P2.
- **Owner's answers at acceptance (2026-11-22):** the editor's user layer is per user and shared
  by all projects (`%APPDATA%\Ironthorn\Editor`, `~/.config/Ironthorn/Editor`), with per-project
  session state in `<project>/Saved/Editor/`. RFC 7396 merge without array operators, the module
  name `file_system`, no command-line layer in P2 and cache writes with `Durability::Atomic` follow
  the engineer's recommendation, accepted by the owner, as does the rest.
- **Revisit when:**
  - the cooker defines where a packaged game's configuration lives;
  - a command-line layer is needed (performance runs, servers);
  - flush cost shows in saves, or a cache needs durability;
  - the Linux CI file system refuses `RENAME_NOREPLACE` often enough that the fallback matters;
  - consoles bring their own save-data APIs.
