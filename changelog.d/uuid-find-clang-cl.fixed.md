- `std::find`, `std::count`, `std::ranges::unique` and similar algorithms over ranges of `Uuid`
  (and of the ids built on it, such as `AssetId` and `StableId`) compile with clang-cl and the
  Visual Studio 18 standard library, which rejected them for 16-byte elements.
