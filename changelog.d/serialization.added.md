- `ith/serialization`: saving and loading any reflected value. `writeText` and `readText` use
  pretty JSON with fields in declaration order, exact integers and floats that read back
  bit-identical; unknown fields are warnings (or errors when cooking), `FormerName` is accepted,
  and comments are allowed only for config files. `writeBinary` and `readBinary` use a versioned
  little-endian blob for cooked data, without `Transient` and `EditorOnly` data, checked by a
  schema hash and read by a hardened reader. Documents carry a format name and version, and text
  documents migrate step by step. Reads are all-or-nothing and report errors with the path of the
  value, e.g. `$.history[1].where.x`, and allocate at most a budget (by default the larger of
  64 MiB and 16 times the input size). See `docs/development/serialization.md`.
- `ith/serialization/Json.h`: `ith::serialization::Value`, a JSON value tree, with `parseJson`
  and `writeJson`.
- The engine now depends on yyjson 0.13.0 (MIT), used only inside the `serialization` module.
- Clang AddressSanitizer builds compile everything with libFuzzer's coverage instrumentation, so
  fuzz targets get coverage feedback from the engine code they reach.
