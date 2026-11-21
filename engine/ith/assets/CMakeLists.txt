# Asset identity and references (ADR-0015): AssetId, SubAssetId and the reflected hard and soft
# reference types. The runtime asset manager and the cooked-data loader join this module later.
# The asset database, importers and sidecars are tools-only and live in asset_database.
ith_add_module(assets
    LAYER core
    SOURCES
        AssetId.h
        AssetRef.h
        AssetRef.cpp
    DEPENDS
        foundation
    REFLECT)
