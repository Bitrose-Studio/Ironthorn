# Layered configuration (ADR-0035): ordered layer directories, settings files in the ith.config
# envelope merged by JSON Merge Patch, reflected settings sections with minimal-difference writes,
# whole files per layer, and generations for change polling. A runtime module: players and
# dedicated servers read configuration too.
ith_add_module(config
    LAYER core
    SOURCES
        Config.h
        Config.cpp
    DEPENDS
        foundation
        file_system
        serialization
        reflection)
