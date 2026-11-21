# Serialization (ADR-0010): the JSON value tree, the text and binary archives driven by
# reflection, document formats and migrations. yyjson is private to this module and included
# only by Json.cpp.
ith_add_module(serialization
    LAYER core
    SOURCES
        ArchiveSupport.h
        ArchiveSupport.cpp
        BinaryArchive.h
        BinaryArchive.cpp
        Document.h
        Error.h
        Error.cpp
        Json.h
        Json.cpp
        TextArchive.h
        TextArchive.cpp
        Value.h
        Value.cpp
    DEPENDS
        foundation
        reflection
    PRIVATE_LIBRARIES
        yyjson)
