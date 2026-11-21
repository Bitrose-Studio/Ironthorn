# The entity-component system: entities, archetype tables, typed queries and the World
# (ADR-0014). Component types are reflected structs; their records come from reflection.
ith_add_module(ecs
    LAYER core
    SOURCES
        Archetype.h
        Archetype.cpp
        ComponentRegistry.h
        ComponentRegistry.cpp
        Entity.h
        Query.h
        World.h
        World.cpp
    DEPENDS
        foundation
        reflection)
