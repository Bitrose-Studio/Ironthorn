- `ith/serialization`: `readText` now refuses a document with an `"id"` unless the caller expects an
  asset document (`TextReadOptions::id`), with `serialization.not_a_document`. An `"id"` used to be
  dropped as an unknown document key with a warning; a document that is not an asset must not have
  one.
