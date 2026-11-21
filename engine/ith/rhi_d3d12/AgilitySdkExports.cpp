// The exports that make an executable load the Direct3D 12 runtime of the Agility SDK
// (ADR-0009) from the D3D12 folder next to it, instead of the runtime built into Windows.
//
// Windows reads them from the executable's export table when the process first uses
// Direct3D 12, so this file is compiled into each executable, never into a library:
// ith_use_d3d12_agility_sdk() (cmake/IronthornDependencies.cmake) adds it and defines
// ITH_D3D12_SDK_VERSION from the pinned package. When the folder or its D3D12Core.dll is
// missing, Windows falls back to its own runtime. The rhi_d3d12 module checks at compile time
// that its headers declare the same version.

#ifndef ITH_D3D12_SDK_VERSION
#error "ITH_D3D12_SDK_VERSION must be defined by ith_use_d3d12_agility_sdk()"
#endif

extern "C" {
// NOLINTBEGIN(readability-identifier-naming): the names are fixed by Direct3D 12
__declspec(dllexport) extern const unsigned int D3D12SDKVersion = ITH_D3D12_SDK_VERSION;
__declspec(dllexport) extern const char* const D3D12SDKPath = ".\\D3D12\\";
// NOLINTEND(readability-identifier-naming)
}
