# Windows, the platform event loop and the clipboard on top of SDL (ADR-0012). A client module:
# a dedicated server has no windows (ADR-0017). SDL is linked privately and never appears in a
# public header.
ith_add_module(platform
    LAYER platform
    KIND client
    SOURCES
        Event.h
        NativeWindowHandle.h
        Platform.h
        Platform.cpp
        PlatformState.h
        Window.h
        Window.cpp
    DEPENDS
        foundation
    PRIVATE_LIBRARIES
        SDL3::SDL3-static)
