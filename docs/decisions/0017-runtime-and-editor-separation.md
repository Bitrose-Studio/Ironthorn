# ADR-0017: Runtime and editor separation

- **Status:** Accepted
- **Date:** 2026-11-20
- **Related:** [ADR-0001](0001-cpp-language-and-safety-rules.md) (build configurations, no RTTI
  in runtime modules), [ADR-0004](0004-modules-and-layering.md) (layers, the runtime never
  depends on the editor), [ADR-0007](0007-logging-and-profiling.md) (what shipping strips),
  [ADR-0008](0008-editor-familiar-to-unreal-developers.md) (Play In Editor, Simulate, Possess /
  Eject), [ADR-0011](0011-shader-language-and-pipeline.md) (tools-only shader compiler,
  `IRONTHORN_BUILD_TOOLS`), [ADR-0012](0012-windowing-and-event-loop.md) (windows, headless),
  [ADR-0013](0013-input.md) (Play In Editor input), [ADR-0014](0014-ecs-storage-and-world.md)
  (world id in handles, Play In Editor world copy), [ADR-0015](0015-asset-identity-and-reimport.md)
  (tools-only asset database, importers and cooker), [ADR-0016](0016-gameplay-model-on-the-ecs.md)
  (game modules, authority as data, hot reload boundary)

## Context

ADR-0004 says the runtime never depends on the editor, and ADRs 0011 and 0015 keep the shader
compiler, importers, asset database and cooker out of games. What is still missing:

- which executables exist and what each one links;
- how a module says where it belongs (a dedicated server has no renderer, audio or UI), and how
  that is checked, not just reviewed;
- how editor-only data is marked and kept out of shipped games;
- how game code runs inside the editor (Play In Editor, Simulate) and how close that is to the
  shipped game.

The editor shell (P2) and the first player sample (P1) need this. Lessons from earlier engine
prototypes by the same maintainer:

- A mechanical layer check stopped the editor boundary from eroding through convenience
  includes. Keeping editor metadata out of runtime types (in editor-side registries, not behind
  a compile-time "editor" switch in runtime code) kept the player and editor builds of the same
  type identical.
- Separate editor and player executables over shared static libraries were simple and worked;
  the player loaded only precompiled, cooked data.

## Options considered

- **Separate executables over shared modules, tagged by kind** (chosen). Each module is compiled
  once per configuration and linked into every target that may have it. A module kind decides
  which targets may link it, and the build checks it.
- **One binary with runtime flags** (`-game`, `-server`). One build, but every game ships the
  editor, the shader compiler, the importers and their licenses (the Windows DXC validator is
  licensed for tools only); a dedicated server drags in display libraries; separation can't be
  proven. Rejected.
- **Runtime modules compiled twice** (an editor flavour and a game flavour, behind a
  `WITH_EDITOR`-style switch). Doubles build time, lets Play In Editor run different code from
  the game, and lets type layouts differ between flavours, which breaks cooked data. Rejected.
- **The editor as a separate process driving the game over IPC.** Crash isolation and a
  true-to-shipping game, but every Outliner refresh, Details edit, selection and possess crosses
  a process boundary with its own serialization; editing during play, which ADR-0008 requires,
  becomes a remote protocol. Rejected as the Play In Editor model; kept only as Standalone launch
  (rule 15), which needs no live link.
- **Play In Editor by snapshot and restore of the editor world.** Gameplay mutates the edited
  world and a restore must catch everything; state outside the world (resources, loaded assets)
  is easy to miss. ADR-0014 already chose a world copy. Rejected.

## Decision

### Module kinds

1. Every module declares a **kind** next to its layer:
   `ith_add_module(<name> LAYER <layer> KIND <kind> …)`, default `runtime`.

   | Kind | Holds | Example |
   |---|---|---|
   | `runtime` | code every game process needs | ECS, asset loader, serialization, gameplay, net |
   | `client` | code a dedicated server doesn't run | window, RHI, renderer, audio, game UI |
   | `tools` | offline processing, never in a game | shader compiler, importers, asset database, cooker |
   | `editor` | the editor | viewport, Outliner, Details, Content Browser, a game's editor module |

   `runtime` and `client` modules live in layers `foundation` to `framework`; `tools` and
   `editor` modules live in the `editor` layer. Game modules use the same kinds.
2. **Allowed dependencies by kind** (on top of ADR-0004's layer rules):
   `runtime` → `runtime`; `client` → `runtime`, `client`; `tools` → `runtime`, `tools`;
   `editor` → any kind. The `module_layering` test checks kinds as it checks layers.
   There is no `server` kind for now: code a server needs is `runtime`. `tools` modules don't
   depend on `client` modules, so tools run headless; GPU bakes run in the editor.

### Targets

3. Executables are declared with `ith_add_application(<name> KIND <target> MODULES …)` in the
   `application` layer:

   | Target | Links kinds | Configurations |
   |---|---|---|
   | **Editor** (one per project) | all | Debug, Development |
   | **Player** (one per project) | `runtime`, `client` | Debug, Development, Shipping |
   | **Dedicated server** | `runtime` | Debug, Development, Shipping |
   | **Tools** (cooker, command-line tools) | `runtime`, `tools` | Debug, Development |

   The same `runtime` module binary goes into every target of a configuration. There is no
   compile-time "editor" switch in runtime or client code. Each project builds its own editor
   executable, linking its game modules statically, until the hot reload ADR says otherwise.
4. **Enforcement, three checks:**
   - the `module_layering` test (rule 2);
   - a **target check** in the same test: the full dependency closure of each application may
     only contain the kinds its row allows;
   - a **shipping link test** in CI: the sample player and server are configured and built in
     Shipping with `IRONTHORN_BUILD_EDITOR=OFF` and `IRONTHORN_BUILD_TOOLS=OFF`, so editor and
     tools modules don't even exist; the server job also sets `IRONTHORN_BUILD_CLIENT=OFF` on a
     Linux image without display packages. If either target needed such a module it would fail
     to link.
5. **Module registration is explicit.** Each application calls the registration function
   (ADR-0016 rule 2) of every module it links, from a source file the build generates out of
   the module list. Nothing relies on static initializers, which static libraries drop.

### Configurations

6. Configurations are those of ADR-0001 and the build today: **Debug** (`ITH_DEBUG`, full
   checks), **Development** (CMake `RelWithDebInfo`: optimized, asserts on) and **Shipping**
   (CMake `Release`, `ITH_SHIPPING`). The CMake names stay, because IDEs and tools know them;
   the documentation uses the engine names. A configuration is how a target is compiled; a target is
   what it contains. They are independent, except that the editor and tools are never built in
   Shipping.
7. **Shipping compiles out:** asserts (`ITH_ENABLE_ASSERTS` off), `Trace` and `Debug` log
   statements (ADR-0007; `Info` and above stay), container access checks, and development-only
   features such as developer console commands, debug drawing and stat overlays. Packaging
   refuses builds with profiling on (ADR-0007).

### Editor-only data

8. **Editor-only data never lives in runtime types.** A runtime component has the same layout in
   every target. Editor data about an entity (label, Outliner folder, editor icon, notes) is a
   separate **editor-only component**, declared in an `editor` module and marked `EditorOnly` in
   reflection; registering an `EditorOnly` type from another kind is an assert. A runtime
   component never has editor-only fields.
9. Source files keep editor-only components in a separate `editor` section of each entity, so
   the cooker drops the section without knowing its types and never treats it as unknown data
   (ADR-0010). Asset authoring data (a material graph, a texture source, import settings) is
   source or sidecar data (ADR-0015); the cooker writes the cooked form, which has no editor
   fields. Cooked data therefore never contains editor-only data, and a CI test cooks the sample
   and loads it in the Shipping player.

### Play In Editor

10. **Play In Editor runs in the editor process, in its own world.** Pressing Play copies the
    editor world into a new world (ADR-0014 rule 4) with a new world id, without editor-only
    components, so the copy looks like what the cooker would produce. The editor world is never
    simulated. Stop sends `WorldEnding` (ADR-0016) and destroys the copy, so nothing needs
    restoring.
11. **Game code is the same code.** The editor links the project's game modules and plugins.
    Their systems are added only to game worlds (Play In Editor, Simulate, the player); the
    editor world runs engine systems needed for display and editor systems. A system may opt in
    to editor worlds (for previews). Each Play In Editor session gets fresh app-scoped resources
    (ADR-0016 rule 6), discarded on Stop. **Game modules keep no mutable global state**, because
    a session can run many times in one process.
12. **Where it plays:** in the **selected viewport** (default) or a **new editor window**
    (ADR-0012). Input follows ADR-0013: Esc stops, Shift + F1 frees the mouse, F8 possesses or
    ejects as the data operations of ADR-0016 rule 9.
13. **Simulate** runs the copied world without creating a local player: gameplay, physics and AI
    run, the editor camera, selection and Details stay available. Play and Simulate switch by
    possess and eject. Details edits during play change the play world. **Keep Simulation
    Changes** on selected entities copies their component values back into the editor world,
    matched by stable object id (ADR-0014 rule 3), as one undoable command. It covers only
    entities that exist in the editor world; entities spawned during play are reported and
    skipped. Asset edits during play are real asset edits
    and persist.
14. **Networked Play In Editor** (with networking, P5): several client worlds and a listen or
    dedicated server world in one process, each client in its own window, through the real
    replication code over an in-process transport. Systems registered by `client` modules get an
    automatic "not a dedicated server" run condition, so a dedicated server world in the editor
    runs what the server binary runs. The network role is the world resource of ADR-0016 rule 14.
    Clients or a server in separate processes come later, through the Standalone launcher.

### Standalone and hot reload

15. **Standalone game** launches the Development player as a separate process on the current
    level, after an incremental cook for the host platform (the DDC keeps it fast). It is the
    true-to-shipping test: cooked data, no editor code, no editor-only data. There is no game
    mode of the editor binary on uncooked data. Later, a dedicated server process can be
    launched the same way.
16. **Hot reload** stays at the boundary of ADR-0016 rule 19, editor and Development only. This
    ADR keeps it possible: game modules have one registration function and no global state
    (rules 5 and 11). The mechanism, likely game modules as shared libraries in non-shipping
    builds, is its own ADR in P5. Shipping always links statically.

## Consequences

- Games and servers can't contain editor or tools code or data, and CI proves it per build.
- Play In Editor runs the same binary code as the player and starts fast, but it uses
  editor-loaded, uncooked data; cook problems show in Standalone, which must stay one click.
- Each project builds its own editor executable, so using the editor with game code needs a C++
  toolchain until hot reload brings shared game modules.
- Editor data about entities lives in separate components, slightly more verbose than fields
  behind a switch, with identical layouts everywhere in return.
- Follow-up work: `KIND` in `ith_add_module` and the kind and target checks in
  `CheckModuleLayering.cmake` (`shader_compiler` becomes `tools`); `ith_add_application` with
  generated registration; `IRONTHORN_BUILD_EDITOR` and `IRONTHORN_BUILD_CLIENT`; the shipping
  link and cooked-sample CI jobs with the first player sample; the `editor` section in the level
  format; world copy, Simulate and Keep Simulation Changes with the P2 editor shell.
- **Revisit when:**
  - a game must keep server logic out of client binaries (a `server` kind);
  - an offline tool needs the GPU (a `tools` → `client` dependency);
  - the incremental cook of a test level takes more than about 10 seconds (an uncooked game mode);
  - the hot reload ADR changes how game modules are linked.
