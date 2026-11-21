# The importers of source files (ADR-0015 rule 8): glTF 2.0 now, images and audio later. Each one
# implements ith::Importer from asset_database and reads untrusted input, so each has a fuzz
# target. Tools only: the editor and the cooker link it, a game never does (ADR-0017).
ith_add_module(asset_importers
    LAYER editor
    KIND tools
    SOURCES
        GltfImporter.h
        GltfImporter.cpp
        ImportedModel.h
    DEPENDS
        foundation
        serialization
        asset_database
    REFLECT)
