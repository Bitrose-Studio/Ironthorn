- `AssetDatabase` and `DerivedDataCache` take and return UTF-8 paths (`std::string`) instead of
  `std::filesystem::path`. Sidecars, id rewrites and native assets are now flushed to the disk
  when they are written, so a power loss no longer loses the latest write; derived-data cache
  entries stay unflushed and are rebuilt when lost.
- On Windows the asset database reads files with delete sharing, so its own reads never make a
  replacement or a move fail.
