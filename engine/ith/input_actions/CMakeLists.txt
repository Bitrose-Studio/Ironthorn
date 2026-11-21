# The input action layer (docs/specs/input-actions.md): input actions and contexts, modifiers and
# triggers, per-player context stacks with key claims, rebinding profiles in layered configuration,
# and the evaluation that writes action states and events for local players on the ECS. A runtime
# module like input: a dedicated server links it and evaluates nothing, having no local players.
ith_add_module(input_actions
    LAYER framework
    SOURCES
        InputActionAssets.h
        InputActionComponents.h
        InputActionComponents.cpp
        InputActions.h
        InputActions.cpp
        InputAssetLibrary.h
        InputAssetLibrary.cpp
        InputBindings.h
        InputBindings.cpp
    DEPENDS
        foundation
        input
        ecs
        assets
        serialization
    REFLECT)
