# The rendering hardware interface (ADR-0009): devices, handles, deferred destruction, frames,
# swapchains, command lists and explicit barriers, with backends behind an internal interface
# (Backend.h, ADR-0021). A client module: a dedicated server doesn't render (ADR-0017). It holds the
# backend-neutral code and the null backend, and no graphics-API header or library; the
# Direct3D 12 and Vulkan backends are separate modules.
ith_add_module(rhi
    LAYER platform
    KIND client
    SOURCES
        Backend.h
        Barrier.h
        Capabilities.h
        CommandList.h
        CommandList.cpp
        Device.h
        Device.cpp
        DeviceState.h
        Error.h
        Error.cpp
        Flags.h
        Format.h
        Format.cpp
        Handle.h
        NullDevice.h
        NullDevice.cpp
        Pipeline.h
        Resources.h
    # platform is in the same layer; rhi uses only its NativeWindowHandle.h, to create
    # swapchains from a window without depending on the windowing library.
    DEPENDS
        foundation
        platform)
