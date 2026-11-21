# Reflection records and the type registry (ADR-0018). The records of annotated types are
# written by tools/ith_reflect into each module that sets REFLECT in ith_add_module.
ith_add_module(reflection
    LAYER core
    SOURCES
        Annotations.h
        TypeInfo.h
        TypeInfo.cpp
        TypeOf.h
        TypeRegistry.h
        TypeRegistry.cpp
    DEPENDS
        foundation)
