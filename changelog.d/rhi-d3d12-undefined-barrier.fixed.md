- On Direct3D 12, a texture barrier out of the `Undefined` layout that names the accesses before
  it (as Vulkan needs for a depth buffer reused every frame) no longer fails the command list.
