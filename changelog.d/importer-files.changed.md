- `ith::Importer::import()` takes the files the importer named in `dependencies()` as a new
  `std::span<const ith::ImportFile>` parameter before the settings. Importers override the new
  signature; those that read no other files ignore it.
