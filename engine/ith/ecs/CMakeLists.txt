# The entity-component system: entities, archetype tables, typed and dynamic queries, resources
# and the World (ADR-0014). Component and resource types are reflected structs.
ith_add_module(ecs
    LAYER core
    SOURCES
        Archetype.h
        Archetype.cpp
        ComponentRegistry.h
        ComponentRegistry.cpp
        DynamicQuery.h
        Entity.h
        Query.h
        World.h
        World.cpp
    DEPENDS
        foundation
        reflection)
