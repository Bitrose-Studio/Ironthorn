# The asset database of a project (ADR-0015): sidecars, native asset headers, the scan of the
# Content folder, move, rename and delete, the importer interface with import and non-destructive
# reimport, and the local derived-data cache. Tools only: the editor and the cooker link it, a game
# never does (ADR-0017).
ith_add_module(asset_database
    LAYER editor
    KIND tools
    SOURCES
        AssetDatabase.h
        AssetDatabase.cpp
        AssetHeader.h
        AssetHeader.cpp
        DerivedDataCache.h
        DerivedDataCache.cpp
        Hash128.h
        Hash128.cpp
        ImportedAssetData.h
        Importer.h
        Importer.cpp
        Sidecar.h
        Sidecar.cpp
    DEPENDS
        foundation
        file_system
        assets
        serialization
        level
    PRIVATE_LIBRARIES
        ith::xxhash
    REFLECT)
