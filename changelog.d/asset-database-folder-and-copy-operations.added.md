- `AssetDatabase` creates, moves and deletes folders (`createFolder`, `moveFolder`,
  `removeFolder`): a folder move keeps the ids of every asset inside, and a folder delete goes on
  after a failure and reports what is left, including sidecars left behind as orphans.
- `AssetDatabase::duplicate` copies an asset with its sidecar under a new id, and
  `AssetDatabase::copyIn` copies a file from outside the project with the files its importer
  reads, keeps a native asset's id unless it is taken, and can replace the source of an imported
  asset.
- `AssetDatabase::findUsers` finds the native assets and sidecars that refer to given assets, and
  the free function `collectAssetReferences` finds the asset references in any JSON value.
- `findCaseVariant`, the database's rule for a taken name, is public.
