- `ith/asset_importers` (tools only): `ith::GltfImporter` imports glTF 2.0 `.gltf` and `.glb`
  files: buffers in the GLB `BIN` chunk, base64 `data:` URIs or files next to the source, sparse,
  interleaved and normalized accessors, triangle meshes (positions, normals, first texture
  coordinates, indices) as `ith::ImportedMesh` sub-assets and metallic-roughness materials as
  `ith::ImportedMaterial` sub-assets. Meshes without a name are keyed by the name path of the
  node that uses them. Every offset, length, stride and index is checked; names and paths are
  limited to 4 KiB; and everything the import builds, copies of names and paths included, is
  limited to 16 times the input (at least 64 MiB, at most 4 GiB).
- `ith::Importer::dependencies()` names the files an importer reads besides its source; the asset
  database reads them from the source's folder, includes them in the derived-data key and passes
  them to `import()`. `ImportOutput::references` and `ImportReport::references` list the files the
  imported objects refer to, such as a material's images.
