- A click in an editor viewport selects the shape under the cursor, and a click on empty space
  clears the selection; Ctrl + click toggles a shape in or out of the selection and Shift + click
  adds it. A click is a left press and release that moves at most 4 points, the same physical
  distance at any display scale; right, middle and Alt drags keep moving the camera without
  selecting, and switching away from the editor with the button held never selects. The selection
  follows two frames after the click and maps correctly on high-DPI displays.
- The RHI has the `R32G32Uint` format (two 32-bit unsigned integers, 8 bytes per texel) on
  Direct3D 12, Vulkan and the null device, and `rhi::isIntegerFormat()`. Integer colour targets
  clear only to 0 (checked when recording in Debug and development builds), and pipeline creation
  refuses blending into them.
