# Windows, the platform event loop, device input and the clipboard on top of SDL (ADR-0012,
# ADR-0013). A client module: a dedicated server has no windows or input devices (ADR-0017). The
# event types it produces live in the runtime module platform_events. SDL is linked privately and
# never appears in a public header.
ith_add_module(platform
    LAYER platform
    KIND client
    SOURCES
        NativeWindowHandle.h
        Platform.h
        Platform.cpp
        PlatformState.h
        Window.h
        Window.cpp
    DEPENDS
        foundation
        platform_events
    PRIVATE_LIBRARIES
        SDL3::SDL3-static)
