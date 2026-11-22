# Level documents (ADR-0034): saveLevel and loadLevel between a World and an "ith.level" text
# document, with the entity mapping to stable ids and UnresolvedComponents for components whose
# type is missing. Between ecs and serialization, so that neither depends on the other.
ith_add_module(level
    LAYER core
    SOURCES
        Level.h
        Level.cpp
        LevelFormat.h
        UnresolvedComponents.h
    DEPENDS
        foundation
        reflection
        serialization
        ecs
    REFLECT)
