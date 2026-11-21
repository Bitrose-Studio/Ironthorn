# What the platform layer emits (ADR-0012, ADR-0013): the engine's event type and the stable ids of
# keys, buttons and axes that device events carry. Header-only and standard library only. A
# runtime module, so the input module and gameplay code that reads input link into a dedicated
# server too; producing events (SDL) stays in the client module `platform`.
ith_add_module(platform_events
    LAYER platform
    SOURCES
        Event.h
        InputCodes.h)
