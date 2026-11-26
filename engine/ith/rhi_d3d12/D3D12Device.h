#pragma once

// The Direct3D 12 backend of the RHI (ADR-0009, ADR-0021). Windows only.
//
//   ith::Expected<ith::rhi::Device, ith::rhi::Error> device = ith::rhi::createD3D12Device({});
//
// Applications don't call this directly: the rhi_select module picks the backend
// (ith/rhi_select/CreateDevice.h). The executable must run on the Agility SDK, whose
// D3D12Core.dll ith_use_d3d12_agility_sdk() puts next to it (docs/development/rhi.md); player
// and editor applications that link rhi_select get it from ith_add_application.
//
// No Direct3D 12 or DXGI type appears in this header.

#include "ith/foundation/Expected.h"
#include "ith/rhi/Device.h"
#include "ith/rhi/Error.h"

namespace ith::rhi {

// Creates a Direct3D 12 device on the GPU desc.adapter names: the first hardware adapter in
// DXGI's high-performance order on which a feature level 12_0 device can be created, or WARP
// (AdapterPreference::Software). The device reports Backend::D3D12 and takes DXIL shaders.
//
// With desc.validation the debug layer is enabled (and GPU-based validation with
// desc.gpuBasedValidation); its warnings and errors are written to stderr, and errors and
// corruption are asserted. Shipping builds ignore both settings and never load the layer.
// DRED's breadcrumbs and page-fault reports are on in every build, for the message of a
// device-lost error.
//
// The debug layer and DRED are process-wide: once a device has enabled them, later devices in the
// process get them too, whatever their own DeviceDesc says, and Direct3D 12 removes devices that
// already exist when a later one turns the debug layer on. Create every device of a process
// with the same validation setting.
//
// Fails with:
// - BackendUnavailable when Direct3D 12 or DXGI can't start; when the process runs an older
//   Direct3D 12 runtime than the Agility SDK's (the executable lacks the exports or its
//   D3D12\D3D12Core.dll); or when validation was asked for and the debug layer can't be loaded,
//   which needs the Windows optional feature "Graphics Tools" besides d3d12SDKLayers.dll;
// - UnsupportedDevice when no GPU supports feature level 12_0, or the chosen GPU lacks a
//   minimum feature of ADR-0005, each of which the message names: Shader Model 6.6, Resource
//   Binding Tier 3, wave intrinsics and enhanced barriers;
// - OutOfMemory or BackendFailure when creating the device's own objects fails.
//
// Must be called on the thread that will be the render thread.
Expected<Device, Error> createD3D12Device(const DeviceDesc& desc);

// For tests of device loss: removes the Direct3D 12 device as a driver reset or hang would
// (ID3D12Device5::RemoveDevice), so that the next submit, wait or present reports DeviceLost.
// Other devices of the process on the same adapter may be removed with it.
// Precondition (asserted): `device` came from createD3D12Device() and wasn't moved from.
void removeD3D12DeviceForTesting(Device& device);

// For tests of a failed submission: makes the next signal of the device's timeline (in submit()
// or endFrame()) fail. Work queued before it can't be tracked then, so the device is removed and
// the call reports DeviceLost. Precondition (asserted): as removeD3D12DeviceForTesting().
void failNextD3D12SignalForTesting(Device& device);

// For tests of GPU zones: whether the device turned its GPU zones off for good, after a command
// list with zones never ran (docs/development/rhi.md, "GPU zones"); false without profiling or
// while the device has no profiler. Precondition (asserted): as removeD3D12DeviceForTesting().
[[nodiscard]] bool d3d12GpuZonesDisabledForTesting(Device& device);

} // namespace ith::rhi
