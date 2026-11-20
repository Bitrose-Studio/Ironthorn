# ADR-0008: An editor familiar to Unreal Engine developers

- **Status:** Accepted
- **Date:** 2026-11-20
- **Related:** [ADR-0003](0003-clean-room.md) (clean room),
  [ADR-0004](0004-modules-and-layering.md) (editor/runtime separation),
  [architecture: World model](../architecture.md#world-model) and
  [Editor](../architecture.md#editor)

## Context

Most developers who will use Ironthorn already know Unreal Engine's editor. Their speed comes
from muscle memory: where panels are, what they are called and which keys do what. An editor
that rearranges all of this costs every new user weeks before they are productive, and gains
nothing in return. The editor shell is built in phase P2, so its layout, vocabulary and default
controls must be fixed before then.

Workflow conventions, panel layout and generic vocabulary are not protected by the engine's
license. Code, icons, styles, art and product trademarks are (ADR-0003).

## Options considered

- **Follow Unreal Engine's editor conventions** for layout, generic vocabulary and default
  controls, with Ironthorn's own implementation and visual identity. Day-one productivity for
  the largest group of users. The cost is that every deliberate difference must be documented.
- **Design an editor workflow from scratch.** Free of any reference, but every user relearns
  basic navigation, and design time goes into problems that are already solved.
- **Follow another editor** (Unity, Godot, a DCC tool). Familiar to a different, smaller group
  of the intended users.

## Decision

1. **Goal.** An Unreal Engine developer is productive in the Ironthorn editor on day one: muscle
   memory, panel names and workflow carry over. Where Ironthorn deliberately differs, the
   difference is listed below and in the editor documentation.
2. **Default layout:**
   - the viewport in the centre;
   - a toolbar on top with Play / Simulate, a place-actor menu and build;
   - the Outliner at the top right, with Details below it;
   - the Content Browser at the bottom, also available as a pop-up content drawer.

   All panels are dockable tabs. Layouts can be saved, and a command resets the default layout.
3. **Vocabulary** for generic concepts: Level, Actor, Component, Outliner, Details, Content
   Browser, Play In Editor, Simulate, Possess / Eject, World Settings, Project Settings and
   Editor Preferences. An **Actor** is how the editor presents an ECS entity with a transform
   ([World model](../architecture.md#world-model)).
4. **Default viewport controls:**

   | Input | Action |
   |---|---|
   | RMB + W/A/S/D, Q/E | Fly; Q/E move down/up |
   | Mouse wheel while RMB is held | Change fly speed |
   | Alt + LMB / Alt + RMB / MMB | Orbit / dolly / pan |
   | F | Focus the selection |
   | W / E / R, Space | Translate / rotate / scale gizmo; Space cycles them |
   | End | Snap the selection to the floor |
   | Alt + drag, Ctrl + D | Duplicate |
   | G | Game view |
   | F11 | Immersive viewport |
   | Ctrl + Space | Open the content drawer |

   During Play In Editor: Esc stops, Shift + F1 frees the mouse, and F8 possesses or ejects.
   Changes made while simulating can be kept.

   Every binding is remappable. This set is the default preset.
5. **Details panel** is generated from reflection metadata: categories, edit permissions, value
   clamps and tooltips. Editing works during Play In Editor.
6. **Content Browser** mirrors the folders on disk. It supports drag-and-drop import, reimport,
   rename and move.
7. **Layered configuration:** engine defaults, then project, then platform, then user. A later
   layer overrides an earlier one.
8. **Legal boundary** (ADR-0003). Only workflow conventions, layout and generic vocabulary are
   followed. No Epic Games source code, icons, styles or art is used. Epic product and feature
   trademarks are not used as names of Ironthorn features; for example, Blueprint, Slate, UMG,
   Niagara, Lumen, Nanite and MetaHuman are not Ironthorn names. The editor has Ironthorn's own
   visual identity.
9. **Deliberate differences:**

   | Area | Unreal Engine | Ironthorn | Why |
   |---|---|---|---|
   | Asset moves and renames | Leave redirector files | No redirectors | Assets have stable ids |
   | Units and axes | Centimetres, +Z up, left-handed | Metres, +Y up, right-handed | [ADR-0006](0006-math-and-coordinate-conventions.md); the editor can display familiar units |
   | Actor | An object class | An editor view of an ECS entity | [World model](../architecture.md#world-model) |
   | Look and names of features | Epic's | Ironthorn's own | ADR-0003 |

10. **Out of scope**, for later ADRs: the editor UI toolkit, visual scripting and hot reload.

## Consequences

- Unreal Engine developers need little training. Developers from other engines learn one
  common, well-documented convention.
- Input remapping, dockable tabs and saved layouts are requirements of the P2 editor shell, not
  later polish.
- Every deliberate difference has to be documented and kept current. The documentation gains an
  "Ironthorn for Unreal developers" mapping page (planned).
- New editor features are checked against this ADR: where an Unreal convention exists and the
  legal boundary allows it, it is the default.
- **Revisit when:** user feedback shows a convention hurts more than it helps, or the editor UI
  toolkit ADR finds a layout requirement it cannot meet.
