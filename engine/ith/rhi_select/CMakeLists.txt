# Picks and creates the RHI backend (ADR-0021): Direct3D 12 by default on Windows, otherwise
# Vulkan. The one module that depends on the backend modules, so that applications, the renderer
# and the editor depend on rhi alone. A client module like the backends.
set(ith_rhi_backends "")
if(WIN32)
    list(APPEND ith_rhi_backends rhi_d3d12)
endif()

ith_add_module(rhi_select
    LAYER platform
    KIND client
    SOURCES
        CreateDevice.h
        CreateDevice.cpp
    DEPENDS
        foundation
        rhi
        ${ith_rhi_backends})
