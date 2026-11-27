- The editor viewport shows a translate gizmo at the primary selected entity when the Translate
  tool is active: three axis arrows (X red, Y green, Z blue) and three plane squares, the same
  size on screen at any distance, with the handle under the pointer highlighted. Dragging a
  handle moves the selection along that axis or within that plane with a live preview, and the
  whole drag is one undo entry, "Translate"; Esc during the drag, or switching away from the
  editor, puts everything back. Alt at the press drags copies of the selection instead, in the
  same single undo entry; an Alt press released without moving leaves no copies. The drag snaps to
  the toolbar's step (0.1 m by default) along each gizmo axis, the World / Local toggle aligns the
  axes with the world or with the primary's rotation, and entities under rotated or scaled
  parents move correctly in the world. An axis seen nearly end-on, or a plane seen nearly
  edge-on, fades out and can't be grabbed. A press away from the handles, or on the pivot itself,
  still selects as before. The gizmo isn't shown with the Rotate or Scale tool, in game view, or
  while another edit (such as a Details field being typed into) is in progress.
