# Input routing (ADR-0013): the device registry, per-frame state, consumer stacks with consumption,
# focus, mouse capture and per-player device assignment. It consumes the engine's events and
# never calls the platform layer, so it is a runtime module: gameplay modules and the action and
# mapping plugin that read input link into a dedicated server too, where no events arrive.
ith_add_module(input
    LAYER core
    SOURCES
        ControlNames.h
        ControlNames.cpp
        Input.h
        Input.cpp
        InputState.h
        InputState.cpp
    DEPENDS
        foundation
        platform_events)
