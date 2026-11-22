# The entity-component system: entities, archetype tables, typed and dynamic queries, resources,
# deferred commands, the schedule and the World (ADR-0014). Component and resource types are
# reflected structs.
ith_add_module(ecs
    LAYER core
    SOURCES
        Archetype.h
        Archetype.cpp
        Commands.h
        Commands.cpp
        ComponentRegistry.h
        ComponentRegistry.cpp
        DynamicQuery.h
        Entity.h
        Hooks.h
        Query.h
        Schedule.h
        Schedule.cpp
        World.h
        World.cpp
    DEPENDS
        foundation
        reflection)
