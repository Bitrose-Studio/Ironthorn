- New `editor_ui` module, the editor's UI on Dear ImGui (docking branch 1.93.0): `ith::EditorUi`
  with our own platform backend over engine events (keys go to a text field only while it is
  active, the mouse to ImGui only while it wants it; clipboard, IME and cursor through the
  platform) and our own renderer backend, which draws ImGui as render graph passes on the RHI with
  partial font atlas updates and linear blending. It brings a neutral dark theme from design
  tokens, the Inter, JetBrains Mono and Lucide icon fonts (copied to `fonts/` next to the
  executables), icons by name, an idle policy that draws on demand and otherwise at the Editor
  Preferences rates (10 frames per second, 2 without focus), and the shell: File and Edit menus
  from the command registry with their shortcuts, a dock space and a status bar. Its tests run
  headless on the null device, under sanitizers too.
- `ith::EditorPreferences` (`editor_core`): the `preferences` section of `Editor.json`.
