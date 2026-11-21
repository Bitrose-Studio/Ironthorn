- `ith/serialization`: asset documents carry their asset id in the envelope; `readText` and
  `writeText` read and write it, and the reader decides whether a document must have one.
  `readDocument` and `writeDocument` read and write documents whose data is keyed by names, such
  as configuration.
