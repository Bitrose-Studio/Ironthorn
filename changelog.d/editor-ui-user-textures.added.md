- Editor panels can call `EditorUi::registerUserTexture` to draw an RHI texture of a
  linear-filterable format (`rhi::isLinearFilterable`) with `ImGui::Image`, passing the texture's
  current graph state when it is known, and `EditorUi::importUserTexture` to share it with scene
  passes before the UI renders. Execute the frame's graph before the next `endFrame()`. Call
  `unregisterUserTexture` after the graph has executed and before destroying the texture;
  destroying first is tolerated, but its draws are skipped and unregistration is still required.
