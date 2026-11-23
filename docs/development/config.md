# Configuration

The `config` module (`engine/ith/config`) reads and writes layered configuration: engine
defaults, then project, then platform, then user ([ADR-0008](../decisions/0008-editor-familiar-to-unreal-developers.md)
rule 7). The design is [ADR-0035](../decisions/0035-file-system-configuration-and-main-thread-queue.md)
rules 11 to 21. It is a `runtime` module in the `core` layer on `file_system`, `serialization` and
`reflection`, so players and dedicated servers read configuration too.

## Layers

A `Config` is built from an ordered list of layer directories, lowest first, each with a name and
a writable flag. The application supplies the list; tests and automation pass their own
directories, so they never touch a developer's real user layer.

| # | Layer | Editor directories | Written |
|---|---|---|---|
| 1 | engine | `<exe>/Config/`, then `<exe>/Config/<Platform>/` | never |
| 2 | project | `<project>/Config/` (committed) | only on explicit request |
| 3 | platform | `<project>/Config/<Platform>/` (`Windows`, `Linux`) | only on explicit request |
| 4 | user | `%APPDATA%\Ironthorn\Editor`, `~/.config/Ironthorn/Editor` (`fs::userConfigDirectory()`) | by default |

- The editor's user layer is per user and shared by all projects. Per-project session state (the
  last session's layout, later recent levels and open tabs) is not configuration: the editor
  writes it to `<project>/Saved/Editor/`.
- A player reads layers 1 to 3 from its installation and a user layer at
  `<userConfig>/<Organization>/<Game>/`; a dedicated server has layers 1 to 3.
- There is no command-line layer yet.

```cpp
#include "ith/config/Config.h"

ith::Config config({
    {.name = "engine", .directory = fs::joinPath(exeDir, "Config")},
    {.name = "engine.platform", .directory = fs::joinPath(exeDir, "Config/Windows")},
    {.name = "project", .directory = fs::joinPath(projectDir, "Config"), .writable = true},
    {.name = "platform", .directory = fs::joinPath(projectDir, "Config/Windows"), .writable = true},
    {.name = "user", .directory = userDir, .writable = true},
});
```

## Settings files

`<layer>/<File>.json` is a document in the ADR-0010 envelope with `format` `ith.config` and
`version` 1, whose `data` is an object of named sections:

```json
// Comments are allowed; files the engine writes have none.
{
  "format": "ith.config",
  "version": 1,
  "data": {
    "preferences": { "idleFramesPerSecond": 5 },
    "layouts": { "default": "Wide" }
  }
}
```

- At most 1 MiB (`kMaxConfigFileSize`).
- A file that can't be read, fails to parse, isn't an `ith.config` document, has data that isn't
  an object, or has a key twice in one object is skipped with one `config.file_skipped` warning.
  That layer contributes nothing for the file, and start-up goes on.

### The merge

Each file is merged across the layers, lowest first, with JSON Merge Patch
([RFC 7396](https://www.rfc-editor.org/rfc/rfc7396)), also available as `ith::applyMergePatch`:

- an object over an object merges key by key, recursively;
- any other value, **arrays included**, replaces the value below it whole;
- `null` removes the key.

There are no array operators. Data that layers edit item by item is an object keyed by a stable
id, never an array: the keymap maps a command id to its chords, and `[]` unbinds a command.

## Typed sections

Settings are reflected structs. Their field initializers are the engine defaults, so the engine
layer doesn't repeat them.

```cpp
struct EditorPreferences {
    ITH_TYPE(Struct)
    ITH_FIELD()
    std::int32_t idleFramesPerSecond = 10;
    ITH_FIELD()
    std::int32_t unfocusedFramesPerSecond = 2;
};
constexpr ith::ConfigSection<EditorPreferences> kEditorPreferences{"Editor", "preferences"};

EditorPreferences preferences = config.get(kEditorPreferences);
preferences.idleFramesPerSecond = 5;
ith::Expected<void> saved = config.set(kEditorPreferences, preferences);          // the user layer
ith::Expected<void> shared = config.set(kEditorPreferences, preferences, "project");
```

- **`get`** reads the merged section field by field. A missing field keeps its default. A field
  the struct doesn't have (`config.unknown_field`) and a value of the wrong type
  (`config.wrong_type`, the field keeps its default) warn once; so do unknown fields inside nested
  structs (`serialization.unknown_field`). A field is also found by its `FormerName`.
- **`set`** writes into the target layer (the highest one by default) only the fields that differ
  from the merge of the layers below it, defaults included, and removes the fields that now equal
  them, so a later change of a lower default still reaches the user. Nested structs are written as
  their own differences; arrays whole. Keys of the section that the struct doesn't have are kept,
  existing keys keep their order and new ones are appended, so diffs stay stable. A former name
  in the file is replaced by the field's name. The file is written at once with
  `fs::writeFileAtomic`, durably; on failure nothing changes in memory. Writing into a layer that
  isn't writable is an assert.
- A `std::optional` field that is empty can't override a value below it: a merge patch can't hold
  `null`. It is left out.
- UIs commit a preference when the control is released, not on every frame of a drag.
- `data(file)` gives the merged data of a file for content without a struct (the keymap).

## Whole files

Documents that are not merged live in the layers as whole files:

| Function | What it does |
|---|---|
| `findFile("Layouts/Default.json")` | The highest layer that has the file, with its path. |
| `listFiles("Layouts")` | The union by name, each from the highest layer that has it, with its origin. Temporary files of interrupted writes are left out. |
| `writeFile("user", "Layouts/Mine.json", bytes)` | Writes one file durably into a writable layer. |

A path that is absolute or leaves the layer fails with `config.invalid_path`.

## Change notification

`generation()` changes on every `set`, `writeFile` and `reload()`; `generation(file)` on every
`set` of that settings file (`"Editor"`), every `writeFile` of that whole file
(`"Layouts/Mine.json"`) and every `reload()`. Consumers compare them once per frame and read
again; there are no callbacks. `reload()` reads every file that was read before again; it backs a
Reload command until the file watcher exists.

## Warnings and threading

Warnings are collected once each in `warnings()`; `takeWarnings()` hands them over for logging,
and the same warning doesn't come back until `reload()`. A `Config` is not thread-safe: its owner
on the main thread hands copies of sections to jobs.

## The engine layer

`engine/config/` is copied to `Config/` next to the executables of every build configuration (the
`ith_engine_config` target, in the default build). It holds only engine data that no settings
struct defines:

| File | Content |
|---|---|
| `Editor.json` | The `layouts` section is `ith::EditorLayoutSettings` (`editor_core/LayoutSession.h`, `ith::kEditorLayoutSettings`): `defaultName`, the layout Reset Layout and a first start use (`Default`; [ADR-0040](../decisions/0040-editor-docking-panels-and-layouts.md)). The `preferences` section is `ith::EditorPreferences` (`editor_core/EditorPreferences.h`, `ith::kEditorPreferences`): `idleFramesPerSecond` (10) and `idleFramesPerSecondUnfocused` (2), the editor's idle rates ([editor UI](editor-ui.md#idle-policy)); its defaults are the struct's. |
| `Keymap.json` | `commands`: editor command id to its chords, the ADR-0008 rule 4 preset. A chord is a stable control name (`Keyboard.F`) after optional `Ctrl+`, `Shift+` and `Alt+`. Viewport navigation (fly, orbit, pan) is held input and comes with the viewport. The ids are those of the editor command registry ([editor core](editor-core.md#keymap)); the Play In Editor keys have their own context. |
| `Layouts/Default.json` | The Default layout of ADR-0029 rule 15, an `ith.editor_layout` document: a dock tree of split nodes (`split` `horizontal`, first on the left, or `vertical`, first on top; a `ratio` for the first child; `first` and `second`) and tab nodes (`tabs` of panel ids, the `active` one), and `floating` panels. Viewport in the centre, Outliner top right with Details below, Content Browser at the bottom with the Output Log beside it. |

Input bindings (`Input.json`, an `ith.input_bindings` document per layer) are whole files too: the
application reads each layer's text and passes it to `InputBindingConfig::setLayer`, so
`input_actions` keeps its own merge.

## Tests

`config` (`tests/config`, test settings types reflected into `ith_test_config`):

- the RFC 7396 examples;
- merging across three layers, comments, `null` removal, arrays replaced;
- skipped files: a syntax error, a duplicate key, another format, data that is not an object, a
  file over 1 MiB; one warning each;
- defaults for missing fields, a former name, unknown fields and wrong types warning once,
  `takeWarnings` and `reload`;
- `set`: only the difference written, fields removed when they equal the layers below, a lower
  default reaching the user after `reload`, key order and other keys kept, former names replaced,
  empty optionals, another target layer, a failed write changing nothing, and the assert for a
  layer that isn't writable;
- `findFile`, `listFiles`, `writeFile`, invalid paths, generations and `reload`;
- the shipped engine layer, from `engine/config` and from the copy next to the test executable:
  no warnings, the keymap's shape, the default layout name, and the Default layout's panels.

## Not there yet

- A file watcher (after P2); until then `reload()`.
- A command-line layer (performance runs, servers).
- Where a packaged game's configuration lives (the cooker).
