# The Direct3D 12 backend of the RHI (ADR-0009, ADR-0021). Windows only, and a client module like
# rhi. DirectX-Headers, D3D12MA and the Windows SDK's DXGI are private: no Direct3D type leaves
# the module. AgilitySdkExports.cpp is not part of the library: ith_use_d3d12_agility_sdk()
# compiles it into each executable that runs Direct3D 12.
if(NOT WIN32)
    # Modules may include it behind a platform check; the module_layering test allows that.
    set_property(GLOBAL APPEND PROPERTY ITH_UNAVAILABLE_MODULES rhi_d3d12)
    return()
endif()

ith_add_module(rhi_d3d12
    LAYER platform
    KIND client
    SOURCES
        D3D12Backend.h
        D3D12Backend.cpp
        D3D12CommandList.cpp
        D3D12Device.h
        D3D12Profiler.h
        D3D12Profiler.cpp
    DEPENDS
        foundation
        rhi
    PRIVATE_LIBRARIES
        ith::d3d12ma
        Microsoft::DirectX-Headers
        Microsoft::DirectX-Guids
        d3d12
        dxgi)

if(TARGET ith_rhi_d3d12)
    target_compile_definitions(ith_rhi_d3d12 PRIVATE ITH_D3D12_SDK_VERSION=${ITH_D3D12_SDK_VERSION})
endif()
