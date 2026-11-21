- `ith/assets`: `ith::AssetId` and `ith::SubAssetId`, and the reflected reference types
  `ith::AssetRef` (hard) and `ith::SoftAssetRef` (soft), which save ids only.
- `ith/asset_database` (tools only): `<file>.meta` sidecars, the headers of native assets (`.ithasset`, levels `.ithmap`), and
  `ith::AssetDatabase`, which scans a project's `Content` folder, reports duplicate ids, orphan
  and missing sidecars, case conflicts and unreadable files, moves, renames and deletes assets
  with their sidecars, and imports through the `ith::Importer` interface. Reimport keeps the ids
  of sub-assets by their stable source keys and marks disappeared ones missing. Importer output
  is cached in a local derived-data cache keyed by XXH3-128 content hashes.
