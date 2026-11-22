# The entity-component system: entities, archetype tables, typed and dynamic queries, resources,
# deferred commands, the schedule, hooks, the hierarchy with enabled state and transform
# propagation, and the World (ADR-0014, ADR-0030, ADR-0031). Component and resource types are
# reflected structs; the hierarchy's own components are reflected here.
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
        Hierarchy.h
        Hooks.h
        Query.h
        Schedule.h
        Schedule.cpp
        TransformPropagation.h
        TransformPropagation.cpp
        World.h
        World.cpp
    DEPENDS
        foundation
        math
        reflection
    REFLECT)
