#include "ith/rhi_d3d12/D3D12Backend.h"

#include "ith/foundation/Assert.h"
#include "ith/rhi/Backend.h"
#include "ith/rhi/Pipeline.h"
#include "ith/rhi_d3d12/D3D12Device.h"

#include <D3D12MemAlloc.h>
#include <directx/d3d12.h>
#include <directx/d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The headers must declare the runtime the executables load (ith_use_d3d12_agility_sdk()).
static_assert(D3D12_SDK_VERSION == ITH_D3D12_SDK_VERSION,
              "DirectX-Headers and the Agility SDK must have the same version (cmake/IronthornDependencies.cmake)");

namespace ith::rhi::d3d12 {
namespace {

// The root signature's constants: 32 values of 4 bytes at register b0, space 0 (ADR-0009 rule 9).
constexpr UINT kRootConstantCount = kMaxPushConstantSize / 4;
static_assert(kRootConstantCount <= 64, "a root signature holds at most 64 DWORDs");
// The indirect argument layouts of CommandList.h are Direct3D 12's.
static_assert(sizeof(DrawIndexedIndirectArguments) == sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
static_assert(sizeof(DispatchIndirectArguments) == sizeof(D3D12_DISPATCH_ARGUMENTS));
// Swapchain buffers; flip-model swapchains need at least 2.
constexpr UINT kSwapchainImageCount = 3;
// Whether this build may load the Direct3D 12 debug layer: never in Shipping.
#ifdef ITH_SHIPPING
constexpr bool kDebugLayerAllowed = false;
#else
constexpr bool kDebugLayerAllowed = true;
#endif
static_assert(kSwapchainImageCount <= kMaxSwapchainImages);

std::string hex(HRESULT result) {
    std::array<char, 16> text{};
    std::snprintf(text.data(), text.size(), "0x%08X", static_cast<unsigned int>(result));
    return text.data();
}

Unexpected<Error> fail(ErrorCode code, std::string message) {
    return makeUnexpected(Error{.code = code, .message = std::move(message)});
}

void setName(ID3D12Object* object, std::string_view name) {
    if (object != nullptr) {
        object->SetName(widen(name).c_str());
    }
}

const char* removalReasonName(HRESULT reason) {
    switch (reason) {
        case DXGI_ERROR_DEVICE_HUNG:
            return "DXGI_ERROR_DEVICE_HUNG";
        case DXGI_ERROR_DEVICE_REMOVED:
            return "DXGI_ERROR_DEVICE_REMOVED";
        case DXGI_ERROR_DEVICE_RESET:
            return "DXGI_ERROR_DEVICE_RESET";
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
            return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
        case DXGI_ERROR_INVALID_CALL:
            return "DXGI_ERROR_INVALID_CALL";
        default:
            return "removed";
    }
}

bool isDepthStencil(Format format) {
    return format == Format::D32FloatS8Uint;
}

// The format of a texture's resource: typeless for depth formats, so that they can be viewed as
// depth (DSV) and as floats (SRV).
DXGI_FORMAT resourceFormat(Format format) {
    switch (format) {
        case Format::D32Float:
            return DXGI_FORMAT_R32_TYPELESS;
        case Format::D32FloatS8Uint:
            return DXGI_FORMAT_R32G8X24_TYPELESS;
        default:
            return dxgiFormat(format);
    }
}

// The format of a texture's shader-resource view.
DXGI_FORMAT shaderViewFormat(Format format) {
    switch (format) {
        case Format::D32Float:
            return DXGI_FORMAT_R32_FLOAT;
        case Format::D32FloatS8Uint:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        default:
            return dxgiFormat(format);
    }
}

[[maybe_unused]] bool isSrgb(Format format) {
    return format == Format::R8G8B8A8Srgb || format == Format::B8G8R8A8Srgb;
}

D3D12_COMPARISON_FUNC comparison(CompareOp op) {
    switch (op) {
        case CompareOp::Never:
            return D3D12_COMPARISON_FUNC_NEVER;
        case CompareOp::Less:
            return D3D12_COMPARISON_FUNC_LESS;
        case CompareOp::Equal:
            return D3D12_COMPARISON_FUNC_EQUAL;
        case CompareOp::LessEqual:
            return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case CompareOp::Greater:
            return D3D12_COMPARISON_FUNC_GREATER;
        case CompareOp::NotEqual:
            return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case CompareOp::GreaterEqual:
            return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case CompareOp::Always:
            return D3D12_COMPARISON_FUNC_ALWAYS;
    }
    return D3D12_COMPARISON_FUNC_ALWAYS;
}

D3D12_TEXTURE_ADDRESS_MODE addressMode(AddressMode mode) {
    switch (mode) {
        case AddressMode::Repeat:
            return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case AddressMode::MirroredRepeat:
            return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case AddressMode::ClampToEdge:
            return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case AddressMode::ClampToBorder:
            return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }
    return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
}

D3D12_FILTER_TYPE filterType(Filter filter) {
    return filter == Filter::Linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
}

D3D12_RENDER_TARGET_BLEND_DESC blendState(BlendMode mode) {
    D3D12_RENDER_TARGET_BLEND_DESC blend{};
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    switch (mode) {
        case BlendMode::Opaque:
            blend.SrcBlend = D3D12_BLEND_ONE;
            blend.DestBlend = D3D12_BLEND_ZERO;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlendAlpha = D3D12_BLEND_ZERO;
            return blend;
        case BlendMode::Alpha:
            blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case BlendMode::Premultiplied:
            blend.SrcBlend = D3D12_BLEND_ONE;
            blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case BlendMode::Additive:
            blend.SrcBlend = D3D12_BLEND_ONE;
            blend.DestBlend = D3D12_BLEND_ONE;
            break;
    }
    blend.BlendEnable = TRUE;
    blend.SrcBlendAlpha = blend.SrcBlend == D3D12_BLEND_SRC_ALPHA ? D3D12_BLEND_ONE : blend.SrcBlend;
    blend.DestBlendAlpha = blend.DestBlend;
    return blend;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType(PrimitiveTopology topology) {
    switch (topology) {
        case PrimitiveTopology::TriangleList:
        case PrimitiveTopology::TriangleStrip:
            return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        case PrimitiveTopology::LineList:
            return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        case PrimitiveTopology::PointList:
            return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    }
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
}

D3D_PRIMITIVE_TOPOLOGY topology(PrimitiveTopology topology) {
    switch (topology) {
        case PrimitiveTopology::TriangleList:
            return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case PrimitiveTopology::TriangleStrip:
            return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case PrimitiveTopology::LineList:
            return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case PrimitiveTopology::PointList:
            return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    }
    return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

// Whether `code` is a DXIL shader: a DXBC container (the format DXC writes) with a DXIL part.
bool isDxil(std::span<const std::byte> code) {
    const auto read32 = [&](std::size_t offset) {
        std::uint32_t value = 0;
        std::memcpy(&value, code.data() + offset, sizeof(value));
        return value;
    };
    constexpr std::size_t kHeaderSize = 32; // fourcc, digest, version, size, part count
    if (code.size() < kHeaderSize || std::memcmp(code.data(), "DXBC", 4) != 0 || read32(24) != code.size()) {
        return false;
    }
    const std::uint32_t parts = read32(28);
    if (parts > (code.size() - kHeaderSize) / 4) {
        return false;
    }
    for (std::uint32_t part = 0; part < parts; ++part) {
        const std::uint32_t offset = read32(kHeaderSize + std::size_t{part} * 4);
        if (offset <= code.size() - 8 && std::memcmp(code.data() + offset, "DXIL", 4) == 0) {
            return true;
        }
    }
    return false;
}

std::optional<Error> checkDxil(const ShaderDesc& shader, std::string_view pipelineName) {
    if (isDxil(shader.code)) {
        return std::nullopt;
    }
    return Error{.code = ErrorCode::InvalidShader,
                 .message = "Shader '" + std::string(shader.entryPoint) + "' of pipeline '" +
                            std::string(pipelineName) + "' is not DXIL; Direct3D 12 takes DXIL bytecode"};
}

D3D12_SHADER_BYTECODE bytecode(const ShaderDesc& shader) {
    return D3D12_SHADER_BYTECODE{.pShaderBytecode = shader.code.data(), .BytecodeLength = shader.code.size()};
}

#ifndef ITH_SHIPPING
// Writes the debug layer's warnings and errors to stderr (there is no engine log yet) and asserts
// on errors and corruption, so that validation failures stop tests and development builds.
void CALLBACK onDebugMessage(D3D12_MESSAGE_CATEGORY /*category*/, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
                             LPCSTR description, void* /*context*/) {
    const char* level = "message";
    switch (severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION:
            level = "corruption";
            break;
        case D3D12_MESSAGE_SEVERITY_ERROR:
            level = "error";
            break;
        case D3D12_MESSAGE_SEVERITY_WARNING:
            level = "warning";
            break;
        default:
            break;
    }
    std::fprintf(stderr, "D3D12 %s #%d: %s\n", level, static_cast<int>(id), description);
    ITH_ASSERT_MSG(severity != D3D12_MESSAGE_SEVERITY_CORRUPTION && severity != D3D12_MESSAGE_SEVERITY_ERROR,
                   "the Direct3D 12 debug layer reported an error (see stderr)");
}
#endif

// Turns on DRED's breadcrumbs and page-fault reports for every device created afterwards. They
// cost little and are the only diagnostics after a device loss (ADR-0009 rule 19).
void enableDred() {
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings)))) {
        settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }
}

// The D3D12SDKVersion of the Direct3D 12 runtime the process loaded (0 if it can't be told):
// the Agility SDK's when the executable exports D3D12SDKVersion and D3D12SDKPath and the
// D3D12Core.dll is in place, otherwise the one built into Windows.
UINT loadedRuntimeVersion() {
    // No reference is taken: the device just created keeps D3D12Core.dll loaded.
    const HMODULE core = GetModuleHandleW(L"D3D12Core.dll");
    if (core == nullptr) {
        return 0;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): an exported variable
    const auto* version = reinterpret_cast<const UINT*>(GetProcAddress(core, "D3D12SDKVersion"));
    return version != nullptr ? *version : 0;
}

AdapterInfo adapterInfo(IDXGIAdapter1* adapter) {
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    return AdapterInfo{.name = narrow(static_cast<const wchar_t*>(desc.Description)),
                       .vendorId = desc.VendorId,
                       .deviceId = desc.DeviceId,
                       .dedicatedMemory = software ? 0 : desc.DedicatedVideoMemory,
                       .software = software};
}

// The minimum features of ADR-0005 the device lacks, by name.
std::vector<std::string_view> missingFeatures(ID3D12Device* device) {
    std::vector<std::string_view> missing;
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{.HighestShaderModel = D3D_SHADER_MODEL_6_6};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))) ||
        shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_6) {
        missing.emplace_back("Shader Model 6.6");
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) ||
        options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3) {
        missing.emplace_back("Resource Binding Tier 3");
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1))) ||
        options1.WaveOps == FALSE) {
        missing.emplace_back("wave intrinsics");
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12))) ||
        options12.EnhancedBarriersSupported == FALSE) {
        missing.emplace_back("enhanced barriers");
    }
    return missing;
}

void readOptionalFeatures(ID3D12Device* device, Capabilities& capabilities) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)))) {
        capabilities.rayTracing = options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6, sizeof(options6)))) {
        capabilities.variableRateShading = options6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_2;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7)))) {
        capabilities.meshShaders = options7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1;
        capabilities.samplerFeedback = options7.SamplerFeedbackTier >= D3D12_SAMPLER_FEEDBACK_TIER_0_9;
    }
}

} // namespace

// --- Strings and formats ------------------------------------------------------------------

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(length, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string narrow(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(std::max(length, 1)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), length, nullptr, nullptr);
    utf8.pop_back(); // the terminator
    return utf8;
}

DXGI_FORMAT dxgiFormat(Format format) {
    switch (format) {
        case Format::Unknown:
            return DXGI_FORMAT_UNKNOWN;
        case Format::R8Unorm:
            return DXGI_FORMAT_R8_UNORM;
        case Format::R8G8Unorm:
            return DXGI_FORMAT_R8G8_UNORM;
        case Format::R8G8B8A8Unorm:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::R8G8B8A8Srgb:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case Format::B8G8R8A8Unorm:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::B8G8R8A8Srgb:
            return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case Format::R10G10B10A2Unorm:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case Format::R11G11B10Float:
            return DXGI_FORMAT_R11G11B10_FLOAT;
        case Format::R16G16Float:
            return DXGI_FORMAT_R16G16_FLOAT;
        case Format::R16G16B16A16Float:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::R32Uint:
            return DXGI_FORMAT_R32_UINT;
        case Format::R32G32Uint:
            return DXGI_FORMAT_R32G32_UINT;
        case Format::R32Float:
            return DXGI_FORMAT_R32_FLOAT;
        case Format::R32G32Float:
            return DXGI_FORMAT_R32G32_FLOAT;
        case Format::R32G32B32A32Float:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case Format::D32Float:
            return DXGI_FORMAT_D32_FLOAT;
        case Format::D32FloatS8Uint:
            return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

// --- Creation -----------------------------------------------------------------------------

Expected<std::unique_ptr<D3D12Backend>, Error> D3D12Backend::create(const DeviceDesc& desc) {
    auto backend = std::make_unique<D3D12Backend>();

    // Shipping builds never load the debug layer (ADR-0009 rule 18).
    const bool validation = desc.validation && kDebugLayerAllowed;
#ifndef ITH_SHIPPING
    if (validation) {
        ComPtr<ID3D12Debug1> debug;
        if (const HRESULT result = D3D12GetDebugInterface(IID_PPV_ARGS(&debug)); FAILED(result)) {
            return fail(ErrorCode::BackendUnavailable,
                        "The Direct3D 12 debug layer is not available (" + hex(result) +
                            "): it needs the Windows optional feature \"Graphics Tools\" and the Agility "
                            "SDK's d3d12SDKLayers.dll next to D3D12Core.dll; or turn DeviceDesc::validation off");
        }
        debug->EnableDebugLayer();
        debug->SetEnableGPUBasedValidation(desc.gpuBasedValidation ? TRUE : FALSE);
    }
#endif
    enableDred();

    // The DXGI debug layer comes with the Windows "Graphics Tools"; without them, plain DXGI.
    if (!validation || FAILED(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&backend->m_factory)))) {
        if (const HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&backend->m_factory)); FAILED(result)) {
            return fail(ErrorCode::BackendUnavailable, "DXGI can't start (" + hex(result) + ")");
        }
    }

    if (desc.adapter == AdapterPreference::Software) {
        if (const HRESULT result = backend->m_factory->EnumWarpAdapter(IID_PPV_ARGS(&backend->m_adapter));
            FAILED(result)) {
            return fail(ErrorCode::UnsupportedDevice,
                        "WARP, the software rasterizer, is not available (" + hex(result) + ")");
        }
        if (const HRESULT result =
                D3D12CreateDevice(backend->m_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&backend->m_device));
            FAILED(result)) {
            return fail(ErrorCode::UnsupportedDevice,
                        "WARP can't create a Direct3D 12 device at feature level 12_0 (" + hex(result) + ")");
        }
    } else {
        // The fastest hardware GPU first; skip software adapters and GPUs without 12_0.
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT index = 0; SUCCEEDED(backend->m_factory->EnumAdapterByGpuPreference(
                 index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
             ++index) {
            DXGI_ADAPTER_DESC1 adapterDesc{};
            adapter->GetDesc1(&adapterDesc);
            if ((adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                continue;
            }
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&backend->m_device)))) {
                backend->m_adapter = std::move(adapter);
                break;
            }
        }
        if (backend->m_device == nullptr) {
            return fail(ErrorCode::UnsupportedDevice, "No GPU supports Direct3D 12 at feature level 12_0");
        }
    }

    // Without the Agility SDK, Windows' own runtime runs, which may lack what the headers declare.
    if (const UINT runtime = loadedRuntimeVersion(); runtime < D3D12_SDK_VERSION) {
        return fail(ErrorCode::BackendUnavailable,
                    "The Direct3D 12 runtime has SDK version " + std::to_string(runtime) + ", older than " +
                        std::to_string(D3D12_SDK_VERSION) +
                        ": the executable must export D3D12SDKVersion and D3D12SDKPath and have the Agility "
                        "SDK's D3D12Core.dll in its D3D12 folder (ith_use_d3d12_agility_sdk)");
    }

    Capabilities& capabilities = backend->m_capabilities;
    capabilities.backend = Backend::D3D12;
    capabilities.shaderFormat = ShaderFormat::Dxil;
    capabilities.adapter = adapterInfo(backend->m_adapter.Get());
    if (const std::vector<std::string_view> missing = missingFeatures(backend->m_device.Get()); !missing.empty()) {
        return makeUnexpected(detail::unsupportedDeviceError(capabilities.adapter.name, missing));
    }
    readOptionalFeatures(backend->m_device.Get(), capabilities);

#ifndef ITH_SHIPPING
    if (validation && SUCCEEDED(backend->m_device.As(&backend->m_infoQueue))) {
        // Only warnings and worse; clearing a render target with another value than its
        // optimized clear value is normal here, because textures are created without one.
        std::array<D3D12_MESSAGE_SEVERITY, 2> quiet{D3D12_MESSAGE_SEVERITY_INFO, D3D12_MESSAGE_SEVERITY_MESSAGE};
        std::array<D3D12_MESSAGE_ID, 2> ignored{D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
                                                D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE};
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumSeverities = static_cast<UINT>(quiet.size());
        filter.DenyList.pSeverityList = quiet.data();
        filter.DenyList.NumIDs = static_cast<UINT>(ignored.size());
        filter.DenyList.pIDList = ignored.data();
        backend->m_infoQueue->PushStorageFilter(&filter);
        backend->m_infoQueue->RegisterMessageCallback(onDebugMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr,
                                                      &backend->m_messageCallbackCookie);
    }
#endif

    if (Expected<void, Error> initialized = backend->initialize(desc); !initialized) {
        return makeUnexpected(std::move(initialized).error());
    }
    return backend;
}

Expected<void, Error> D3D12Backend::initialize(const DeviceDesc& desc) {
    const Limits& limits = m_capabilities.limits;
    setName(m_device.Get(), desc.applicationName);

    D3D12MA::ALLOCATOR_DESC allocatorDesc{};
    allocatorDesc.Flags = static_cast<D3D12MA::ALLOCATOR_FLAGS>(D3D12MA_RECOMMENDED_ALLOCATOR_FLAGS);
    allocatorDesc.pDevice = m_device.Get();
    allocatorDesc.pAdapter = m_adapter.Get();
    if (const HRESULT result = D3D12MA::CreateAllocator(&allocatorDesc, &m_allocator); FAILED(result)) {
        return makeUnexpected(failure(result, "creating the GPU memory allocator"));
    }

    const D3D12_COMMAND_QUEUE_DESC queueDesc{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    if (const HRESULT result = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue)); FAILED(result)) {
        return makeUnexpected(failure(result, "creating the graphics queue"));
    }
    setName(m_queue.Get(), "Graphics queue");
    if (const HRESULT result = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
        FAILED(result)) {
        return makeUnexpected(failure(result, "creating the graphics queue's fence"));
    }
    setName(m_fence.Get(), "Graphics timeline");
    m_fenceEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (m_fenceEvent == nullptr) {
        return makeUnexpected(failure(HRESULT_FROM_WIN32(GetLastError()), "creating the fence event"));
    }

    // One shader-visible heap per kind (ADR-0009 rule 7), and CPU heaps with a view per texture.
    const auto createHeap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type, std::uint32_t count, bool shaderVisible,
                                ComPtr<ID3D12DescriptorHeap>& heap, std::uint32_t& size, std::string_view name) {
        const D3D12_DESCRIPTOR_HEAP_DESC heapDesc{.Type = type,
                                                  .NumDescriptors = count,
                                                  .Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                                                                         : D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
        const HRESULT result = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap));
        if (SUCCEEDED(result)) {
            size = m_device->GetDescriptorHandleIncrementSize(type);
            setName(heap.Get(), name);
        }
        return result;
    };
    if (const HRESULT result = createHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, limits.resourceDescriptors, true,
                                          m_resourceHeap, m_resourceDescriptorSize, "Bindless resources");
        FAILED(result)) {
        return makeUnexpected(failure(result, "creating the bindless resource heap"));
    }
    if (const HRESULT result = createHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, limits.samplerDescriptors, true,
                                          m_samplerHeap, m_samplerDescriptorSize, "Bindless samplers");
        FAILED(result)) {
        return makeUnexpected(failure(result, "creating the bindless sampler heap"));
    }
    if (const HRESULT result = createHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, limits.textures, false, m_renderTargetHeap,
                                          m_renderTargetDescriptorSize, "Render-target views");
        FAILED(result)) {
        return makeUnexpected(failure(result, "creating the render-target view heap"));
    }
    if (const HRESULT result = createHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, limits.textures, false, m_depthStencilHeap,
                                          m_depthStencilDescriptorSize, "Depth-stencil views");
        FAILED(result)) {
        return makeUnexpected(failure(result, "creating the depth-stencil view heap"));
    }

    // The one root signature: 128 bytes of root constants at b0, and the heaps indexed directly
    // by SM 6.6 shaders (ResourceDescriptorHeap, SamplerDescriptorHeap).
    D3D12_ROOT_PARAMETER1 constants{};
    constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constants.Constants = {.ShaderRegister = 0, .RegisterSpace = 0, .Num32BitValues = kRootConstantCount};
    constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rootDesc.Desc_1_1.NumParameters = 1;
    rootDesc.Desc_1_1.pParameters = &constants;
    rootDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                              D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (const HRESULT result = D3D12SerializeVersionedRootSignature(&rootDesc, &serialized, &errors); FAILED(result)) {
        return makeUnexpected(failure(result, "serializing the root signature"));
    }
    if (const HRESULT result = m_device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature));
        FAILED(result)) {
        return makeUnexpected(failure(result, "creating the root signature"));
    }
    setName(m_rootSignature.Get(), "Bindless root signature");

    const D3D12_INDIRECT_ARGUMENT_DESC drawIndexed{.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED};
    const D3D12_COMMAND_SIGNATURE_DESC drawIndexedDesc{
        .ByteStride = sizeof(DrawIndexedIndirectArguments), .NumArgumentDescs = 1, .pArgumentDescs = &drawIndexed};
    if (const HRESULT result =
            m_device->CreateCommandSignature(&drawIndexedDesc, nullptr, IID_PPV_ARGS(&m_drawIndexedSignature));
        FAILED(result)) {
        return makeUnexpected(failure(result, "creating the indexed indirect draw signature"));
    }
    const D3D12_INDIRECT_ARGUMENT_DESC dispatch{.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
    const D3D12_COMMAND_SIGNATURE_DESC dispatchDesc{
        .ByteStride = sizeof(DispatchIndirectArguments), .NumArgumentDescs = 1, .pArgumentDescs = &dispatch};
    if (const HRESULT result =
            m_device->CreateCommandSignature(&dispatchDesc, nullptr, IID_PPV_ARGS(&m_dispatchSignature));
        FAILED(result)) {
        return makeUnexpected(failure(result, "creating the indirect dispatch signature"));
    }

    BOOL tearing = FALSE;
    m_tearingSupported =
        SUCCEEDED(m_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))) &&
        tearing != FALSE;

    m_buffers.resize(limits.buffers);
    m_textures.resize(limits.textures);
    m_pipelines.resize(limits.pipelines);
    m_swapchains.resize(limits.swapchains);
#ifdef ITH_ENABLE_PROFILING
    m_gpuProfiler.start(m_device.Get(), m_queue.Get());
#endif
    return {};
}

D3D12Backend::~D3D12Backend() {
    // The device has destroyed every object and waited for the GPU (Backend.h).
#ifdef ITH_ENABLE_PROFILING
    // Its query heap and readback buffer belong to the device, and it holds the queue.
    m_gpuProfiler.stop();
#endif
    if (m_infoQueue != nullptr && m_messageCallbackCookie != 0) {
        m_infoQueue->UnregisterMessageCallback(m_messageCallbackCookie);
    }
}

// --- Errors -------------------------------------------------------------------------------

Error D3D12Backend::failure(HRESULT result, std::string_view what) const {
    const HRESULT removal = m_device != nullptr ? m_device->GetDeviceRemovedReason() : S_OK;
    if (removal != S_OK) {
        return lostError(removal);
    }
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET || result == DXGI_ERROR_DEVICE_HUNG) {
        return lostError(result);
    }
    if (result == E_OUTOFMEMORY) {
        return Error{.code = ErrorCode::OutOfMemory, .message = "Out of memory " + std::string(what)};
    }
    return Error{.code = ErrorCode::BackendFailure,
                 .message = "Direct3D 12 failed " + std::string(what) + " (" + hex(result) + ")"};
}

Error D3D12Backend::lostError(HRESULT reason) const {
    std::string message = "The Direct3D 12 device was lost: " + std::string(removalReasonName(reason)) + " (" +
                          hex(reason) + ") on '" + m_capabilities.adapter.name + "'";
    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (m_device == nullptr || FAILED(m_device.As(&dred))) {
        return Error{.code = ErrorCode::DeviceLost, .message = std::move(message)};
    }
    // The command lists the GPU had not finished, and the operation it stopped at.
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
        for (const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode; node != nullptr;
             node = node->pNext) {
            const UINT done = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
            if (done >= node->BreadcrumbCount) {
                continue;
            }
            message += "; DRED: command list '" + narrow(node->pCommandListDebugNameW) + "' stopped after " +
                       std::to_string(done) + " of " + std::to_string(node->BreadcrumbCount) + " operations, at " +
                       "D3D12_AUTO_BREADCRUMB_OP " + std::to_string(static_cast<int>(node->pCommandHistory[done]));
        }
    }
    // The faulting address and the objects at or recently freed from it.
    D3D12_DRED_PAGE_FAULT_OUTPUT pageFault{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault)) && pageFault.PageFaultVA != 0) {
        std::array<char, 32> address{};
        std::snprintf(address.data(), address.size(), "0x%llX", static_cast<unsigned long long>(pageFault.PageFaultVA));
        message += "; DRED: page fault at " + std::string(address.data());
        for (const D3D12_DRED_ALLOCATION_NODE* node = pageFault.pHeadExistingAllocationNode; node != nullptr;
             node = node->pNext) {
            message += ", in '" + narrow(node->ObjectNameW) + "'";
        }
        for (const D3D12_DRED_ALLOCATION_NODE* node = pageFault.pHeadRecentFreedAllocationNode; node != nullptr;
             node = node->pNext) {
            message += ", recently freed '" + narrow(node->ObjectNameW) + "'";
        }
    }
    return Error{.code = ErrorCode::DeviceLost, .message = std::move(message)};
}

void D3D12Backend::removeDevice() {
    ComPtr<ID3D12Device5> device;
    if (SUCCEEDED(m_device.As(&device))) {
        device->RemoveDevice();
    }
}

// --- Descriptors --------------------------------------------------------------------------

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Backend::descriptor(ID3D12DescriptorHeap* heap, std::uint32_t size,
                                                     std::uint32_t index) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * size;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Backend::renderTargetView(std::uint32_t textureSlot) const {
    return descriptor(m_renderTargetHeap.Get(), m_renderTargetDescriptorSize, textureSlot);
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Backend::depthStencilView(std::uint32_t textureSlot) const {
    return descriptor(m_depthStencilHeap.Get(), m_depthStencilDescriptorSize, textureSlot);
}

// --- Buffers ------------------------------------------------------------------------------

Expected<std::byte*, Error> D3D12Backend::createBuffer(std::uint32_t slot, const BufferDesc& desc,
                                                       const detail::BufferDescriptors& descriptors) {
    // Shaders see buffers as raw (ByteAddressBuffer) views of 4-byte words, so the size is
    // rounded up to whole words, unless that would pass the largest buffer size (desc.size is
    // within it, so the sum can't overflow).
    const std::uint64_t rounded = desc.size + (4 - desc.size % 4) % 4;
    const std::uint64_t width = rounded <= m_capabilities.limits.maxBufferSize ? rounded : desc.size;
    D3D12_RESOURCE_DESC1 resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = width;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc = {.Count = 1, .Quality = 0};
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = hasAny(desc.usage, BufferUsage::ShaderWrite) ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                                      : D3D12_RESOURCE_FLAG_NONE;
    D3D12MA::ALLOCATION_DESC allocationDesc{};
    switch (desc.memory) {
        case MemoryType::GpuOnly:
            allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
            break;
        case MemoryType::Upload:
            allocationDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
            break;
        case MemoryType::Readback:
            allocationDesc.HeapType = D3D12_HEAP_TYPE_READBACK;
            break;
    }

    BufferRecord record;
    if (const HRESULT result =
            m_allocator->CreateResource3(&allocationDesc, &resourceDesc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, 0,
                                         nullptr, &record.allocation, IID_NULL, nullptr);
        FAILED(result)) {
        return makeUnexpected(failure(result, "creating buffer '" + std::string(desc.debugName) + "'"));
    }
    record.resource = record.allocation->GetResource();
    record.size = desc.size;
    record.shaderWrite = hasAny(desc.usage, BufferUsage::ShaderWrite);
    setName(record.resource, desc.debugName);
    record.allocation->SetName(widen(desc.debugName).c_str());

    std::byte* mapped = nullptr;
    if (desc.memory != MemoryType::GpuOnly) {
        // Persistently mapped; upload memory is never read by the CPU.
        const D3D12_RANGE nothingRead{.Begin = 0, .End = 0};
        void* pointer = nullptr;
        if (const HRESULT result =
                record.resource->Map(0, desc.memory == MemoryType::Upload ? &nothingRead : nullptr, &pointer);
            FAILED(result)) {
            return makeUnexpected(failure(result, "mapping buffer '" + std::string(desc.debugName) + "'"));
        }
        mapped = static_cast<std::byte*>(pointer);
    }

    const auto words = static_cast<UINT>(width / 4);
    if (descriptors.read != kInvalidDescriptorIndex) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Buffer = {.FirstElement = 0, .NumElements = words, .Flags = D3D12_BUFFER_SRV_FLAG_RAW};
        m_device->CreateShaderResourceView(
            record.resource, &view, descriptor(m_resourceHeap.Get(), m_resourceDescriptorSize, descriptors.read));
    }
    if (descriptors.write != kInvalidDescriptorIndex) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer = {.FirstElement = 0, .NumElements = words, .Flags = D3D12_BUFFER_UAV_FLAG_RAW};
        m_device->CreateUnorderedAccessView(
            record.resource, nullptr, &view,
            descriptor(m_resourceHeap.Get(), m_resourceDescriptorSize, descriptors.write));
    }
    m_buffers[slot] = std::move(record);
    return mapped;
}

void D3D12Backend::destroyBuffer(std::uint32_t slot) {
    m_buffers[slot] = {};
}

// --- Textures -----------------------------------------------------------------------------

Expected<void, Error> D3D12Backend::createTexture(std::uint32_t slot, const TextureDesc& desc,
                                                  const detail::TextureDescriptors& descriptors) {
    // The device refuses sRGB storage textures (Device.cpp): Direct3D 12 has no sRGB UAVs.
    ITH_ASSERT(!(hasAny(desc.usage, TextureUsage::Storage) && isSrgb(desc.format)));
    const bool volume = desc.dimension == TextureDimension::Texture3D;
    D3D12_RESOURCE_DESC1 resourceDesc{};
    resourceDesc.Dimension = volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width = desc.width;
    resourceDesc.Height = desc.height;
    resourceDesc.DepthOrArraySize = static_cast<UINT16>(volume ? desc.depth : desc.layers);
    resourceDesc.MipLevels = static_cast<UINT16>(desc.mipLevels);
    resourceDesc.Format = resourceFormat(desc.format);
    resourceDesc.SampleDesc = {.Count = 1, .Quality = 0};
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (hasAny(desc.usage, TextureUsage::RenderTarget)) {
        resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (hasAny(desc.usage, TextureUsage::DepthStencil)) {
        resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if (!hasAny(desc.usage, TextureUsage::Sampled)) {
            resourceDesc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        }
    }
    if (hasAny(desc.usage, TextureUsage::Storage)) {
        resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    const D3D12MA::ALLOCATION_DESC allocationDesc{.HeapType = D3D12_HEAP_TYPE_DEFAULT};

    TextureRecord record;
    if (const HRESULT result =
            m_allocator->CreateResource3(&allocationDesc, &resourceDesc, D3D12_BARRIER_LAYOUT_COMMON, nullptr, 0,
                                         nullptr, &record.allocation, IID_PPV_ARGS(&record.resource));
        FAILED(result)) {
        return makeUnexpected(failure(result, "creating texture '" + std::string(desc.debugName) + "'"));
    }
    setName(record.resource.Get(), desc.debugName);
    record.allocation->SetName(widen(desc.debugName).c_str());
    record.copyFormat = isDepthFormat(desc.format) ? DXGI_FORMAT_R32_TYPELESS : dxgiFormat(desc.format);
    record.width = desc.width;
    record.height = desc.height;
    record.depth = volume ? desc.depth : 1;
    record.layers = volume ? 1 : desc.layers;
    record.mipLevels = desc.mipLevels;
    record.planes = isDepthStencil(desc.format) ? 2 : 1;
    record.usage = desc.usage;
    ID3D12Resource* resource = record.resource.Get();

    const bool cube = desc.dimension == TextureDimension::TextureCube;
    if (descriptors.sampled != kInvalidDescriptorIndex) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = shaderViewFormat(desc.format);
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (volume) {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            view.Texture3D.MipLevels = desc.mipLevels;
        } else if (cube && desc.layers == 6) {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            view.TextureCube.MipLevels = desc.mipLevels;
        } else if (cube) {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            view.TextureCubeArray.MipLevels = desc.mipLevels;
            view.TextureCubeArray.NumCubes = desc.layers / 6;
        } else if (desc.layers == 1) {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = desc.mipLevels;
        } else {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.MipLevels = desc.mipLevels;
            view.Texture2DArray.ArraySize = desc.layers;
        }
        m_device->CreateShaderResourceView(
            resource, &view, descriptor(m_resourceHeap.Get(), m_resourceDescriptorSize, descriptors.sampled));
    }
    if (descriptors.storage != kInvalidDescriptorIndex) {
        // Mip 0, every layer (cubes as 2D arrays).
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = dxgiFormat(desc.format);
        if (volume) {
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            view.Texture3D.WSize = desc.depth;
        } else if (desc.layers == 1) {
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        } else {
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.ArraySize = desc.layers;
        }
        m_device->CreateUnorderedAccessView(
            resource, nullptr, &view, descriptor(m_resourceHeap.Get(), m_resourceDescriptorSize, descriptors.storage));
    }
    if (hasAny(desc.usage, TextureUsage::RenderTarget)) {
        // Mip 0, layer 0 (CommandList::beginRendering()).
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = dxgiFormat(desc.format);
        if (volume) {
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
            view.Texture3D.WSize = 1;
        } else {
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.ArraySize = 1;
        }
        m_device->CreateRenderTargetView(resource, &view, renderTargetView(slot));
    }
    if (hasAny(desc.usage, TextureUsage::DepthStencil)) {
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = dxgiFormat(desc.format);
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.ArraySize = 1;
        m_device->CreateDepthStencilView(resource, &view, depthStencilView(slot));
    }
    m_textures[slot] = std::move(record);
    return {};
}

void D3D12Backend::destroyTexture(std::uint32_t slot) {
    m_textures[slot] = {};
}

// --- Samplers -----------------------------------------------------------------------------

Expected<void, Error> D3D12Backend::createSampler(std::uint32_t /*slot*/, const SamplerDesc& desc,
                                                  std::uint32_t descriptorIndex) {
    D3D12_SAMPLER_DESC sampler{};
    const D3D12_FILTER_REDUCTION_TYPE reduction =
        desc.compare ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON : D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    if (desc.maxAnisotropy > 1.0f) {
        sampler.Filter = desc.compare ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
    } else {
        sampler.Filter = D3D12_ENCODE_BASIC_FILTER(filterType(desc.minFilter), filterType(desc.magFilter),
                                                   filterType(desc.mipFilter), reduction);
    }
    sampler.AddressU = addressMode(desc.addressU);
    sampler.AddressV = addressMode(desc.addressV);
    sampler.AddressW = addressMode(desc.addressW);
    sampler.MipLODBias = desc.mipLodBias;
    sampler.MaxAnisotropy = static_cast<UINT>(desc.maxAnisotropy);
    sampler.ComparisonFunc = desc.compare ? comparison(desc.compareOp) : D3D12_COMPARISON_FUNC_NONE;
    sampler.MinLOD = desc.minLod;
    sampler.MaxLOD = desc.maxLod;
    m_device->CreateSampler(&sampler, descriptor(m_samplerHeap.Get(), m_samplerDescriptorSize, descriptorIndex));
    return {};
}

void D3D12Backend::destroySampler(std::uint32_t /*slot*/) {
    // A sampler is only its descriptor, which the device frees.
}

// --- Pipelines ----------------------------------------------------------------------------

Expected<void, Error> D3D12Backend::createGraphicsPipeline(std::uint32_t slot, const GraphicsPipelineDesc& desc) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};
    state.pRootSignature = m_rootSignature.Get();
    for (const ShaderDesc& shader : desc.shaders) {
        if (std::optional<Error> problem = checkDxil(shader, desc.debugName)) {
            return makeUnexpected(std::move(*problem));
        }
        (shader.stage == ShaderStage::Vertex ? state.VS : state.PS) = bytecode(shader);
    }
    state.BlendState.IndependentBlendEnable = TRUE;
    for (std::size_t target = 0; target < desc.colorTargets.size(); ++target) {
        state.BlendState.RenderTarget[target] = blendState(desc.colorTargets[target].blend);
        state.RTVFormats[target] = dxgiFormat(desc.colorTargets[target].format);
    }
    state.NumRenderTargets = static_cast<UINT>(desc.colorTargets.size());
    state.SampleMask = 0xFFFF'FFFFu;
    state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    switch (desc.cullMode) {
        case CullMode::None:
            state.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            break;
        case CullMode::Front:
            state.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
            break;
        case CullMode::Back:
            state.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
            break;
    }
    state.RasterizerState.FrontCounterClockwise = desc.frontFace == FrontFace::CounterClockwise ? TRUE : FALSE;
    state.RasterizerState.DepthClipEnable = TRUE;
    // Writing depth needs the depth test on; without a test it always passes.
    state.DepthStencilState.DepthEnable = desc.depth.test || desc.depth.write ? TRUE : FALSE;
    state.DepthStencilState.DepthWriteMask =
        desc.depth.write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    state.DepthStencilState.DepthFunc = desc.depth.test ? comparison(desc.depth.compare) : D3D12_COMPARISON_FUNC_ALWAYS;
    state.DSVFormat = dxgiFormat(desc.depthFormat);
    state.PrimitiveTopologyType = topologyType(desc.topology);
    state.SampleDesc = {.Count = 1, .Quality = 0};

    PipelineRecord record{.topology = topology(desc.topology), .compute = false};
    if (const HRESULT result = m_device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&record.state));
        FAILED(result)) {
        Error error = failure(result, "creating pipeline '" + std::string(desc.debugName) + "'");
        if (error.code == ErrorCode::BackendFailure) {
            // E_INVALIDARG: the shaders don't match the bindless root signature or each other.
            error.code = ErrorCode::InvalidShader;
        }
        return makeUnexpected(std::move(error));
    }
    setName(record.state.Get(), desc.debugName);
    m_pipelines[slot] = std::move(record);
    return {};
}

Expected<void, Error> D3D12Backend::createComputePipeline(std::uint32_t slot, const ComputePipelineDesc& desc) {
    if (std::optional<Error> problem = checkDxil(desc.shader, desc.debugName)) {
        return makeUnexpected(std::move(*problem));
    }
    const D3D12_COMPUTE_PIPELINE_STATE_DESC state{.pRootSignature = m_rootSignature.Get(), .CS = bytecode(desc.shader)};
    PipelineRecord record{.compute = true};
    if (const HRESULT result = m_device->CreateComputePipelineState(&state, IID_PPV_ARGS(&record.state));
        FAILED(result)) {
        Error error = failure(result, "creating pipeline '" + std::string(desc.debugName) + "'");
        if (error.code == ErrorCode::BackendFailure) {
            error.code = ErrorCode::InvalidShader;
        }
        return makeUnexpected(std::move(error));
    }
    setName(record.state.Get(), desc.debugName);
    m_pipelines[slot] = std::move(record);
    return {};
}

void D3D12Backend::destroyPipeline(std::uint32_t slot) {
    m_pipelines[slot] = {};
}

// --- Swapchains ---------------------------------------------------------------------------

Expected<detail::SwapchainImages, Error> D3D12Backend::takeImages(SwapchainRecord& swapchain, std::uint32_t width,
                                                                  std::uint32_t height) {
    for (UINT image = 0; image < kSwapchainImageCount; ++image) {
        TextureRecord record;
        if (const HRESULT result = swapchain.swapchain->GetBuffer(image, IID_PPV_ARGS(&record.resource));
            FAILED(result)) {
            releaseImages(swapchain);
            return makeUnexpected(failure(result, "getting the images of swapchain '" + swapchain.name + "'"));
        }
        setName(record.resource.Get(), swapchain.name + " image " + std::to_string(image));
        record.copyFormat = swapchain.bufferFormat;
        record.width = width;
        record.height = height;
        record.usage = TextureUsage::RenderTarget | TextureUsage::CopyDst;
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = swapchain.viewFormat;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        const std::uint32_t slot = swapchain.imageSlots[image];
        m_device->CreateRenderTargetView(record.resource.Get(), &view, renderTargetView(slot));
        m_textures[slot] = std::move(record);
    }
    return detail::SwapchainImages{
        .count = kSwapchainImageCount, .format = swapchain.format, .width = width, .height = height};
}

void D3D12Backend::releaseImages(SwapchainRecord& swapchain) {
    for (std::uint32_t image = 0; image < swapchain.imageCount; ++image) {
        m_textures[swapchain.imageSlots[image]] = {};
    }
}

Expected<detail::SwapchainImages, Error> D3D12Backend::createSwapchain(std::uint32_t slot, const SwapchainDesc& desc,
                                                                       std::span<const std::uint32_t> imageSlots) {
    const auto* window = std::get_if<Win32WindowHandle>(&desc.window);
    if (window == nullptr || window->hwnd == nullptr) {
        return fail(ErrorCode::SurfaceFailed,
                    "Swapchain '" + std::string(desc.debugName) + "' needs a Win32 window on Direct3D 12");
    }
    SwapchainRecord record;
    record.format = desc.format;
    record.viewFormat = dxgiFormat(desc.format);
    // Flip-model buffers can't be sRGB; their render-target views are.
    switch (desc.format) {
        case Format::B8G8R8A8Srgb:
            record.bufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;
        case Format::R8G8B8A8Srgb:
            record.bufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
        default:
            record.bufferFormat = record.viewFormat;
            break;
    }
    record.vsync = desc.vsync;
    record.flags = m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    record.name = desc.debugName;
    ITH_ASSERT(imageSlots.size() >= kSwapchainImageCount);
    std::copy_n(imageSlots.begin(), kSwapchainImageCount, record.imageSlots.begin());
    record.imageCount = kSwapchainImageCount;

    DXGI_SWAP_CHAIN_DESC1 swapchainDesc{};
    swapchainDesc.Width = desc.width;
    swapchainDesc.Height = desc.height;
    swapchainDesc.Format = record.bufferFormat;
    swapchainDesc.SampleDesc = {.Count = 1, .Quality = 0};
    swapchainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchainDesc.BufferCount = kSwapchainImageCount;
    swapchainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapchainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapchainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapchainDesc.Flags = record.flags;
    auto* hwnd = static_cast<HWND>(window->hwnd);
    ComPtr<IDXGISwapChain1> swapchain;
    if (const HRESULT result =
            m_factory->CreateSwapChainForHwnd(m_queue.Get(), hwnd, &swapchainDesc, nullptr, nullptr, &swapchain);
        FAILED(result)) {
        Error error = failure(result, "creating swapchain '" + std::string(desc.debugName) + "'");
        if (error.code == ErrorCode::BackendFailure) {
            error.code = ErrorCode::SurfaceFailed;
        }
        return makeUnexpected(std::move(error));
    }
    if (const HRESULT result = swapchain.As(&record.swapchain); FAILED(result)) {
        return makeUnexpected(failure(result, "creating swapchain '" + std::string(desc.debugName) + "'"));
    }
    // The windowing layer handles full screen; DXGI must not react to Alt+Enter.
    m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    SwapchainRecord& stored = m_swapchains[slot];
    stored = std::move(record);
    Expected<detail::SwapchainImages, Error> images = takeImages(stored, desc.width, desc.height);
    if (!images) {
        stored = {};
    }
    return images;
}

Expected<detail::SwapchainImages, Error> D3D12Backend::resizeSwapchain(std::uint32_t slot, std::uint32_t width,
                                                                       std::uint32_t height) {
    SwapchainRecord& swapchain = m_swapchains[slot];
    // ResizeBuffers needs every reference to the old buffers released.
    releaseImages(swapchain);
    if (const HRESULT result = swapchain.swapchain->ResizeBuffers(kSwapchainImageCount, width, height,
                                                                  swapchain.bufferFormat, swapchain.flags);
        FAILED(result)) {
        return makeUnexpected(failure(result, "resizing swapchain '" + swapchain.name + "'"));
    }
    return takeImages(swapchain, width, height);
}

void D3D12Backend::destroySwapchain(std::uint32_t slot) {
    releaseImages(m_swapchains[slot]);
    m_swapchains[slot] = {};
}

Expected<detail::AcquireResult, Error> D3D12Backend::acquireImage(std::uint32_t swapchainSlot,
                                                                  std::uint32_t /*frameSlot*/) {
    // Flip-model swapchains hand out their buffers in order; the queue orders the GPU's work on
    // them, so nothing is waited for here.
    if (removed()) {
        return makeUnexpected(lostError(m_device->GetDeviceRemovedReason()));
    }
    return detail::AcquireResult{.status = detail::AcquireStatus::Acquired,
                                 .imageIndex = m_swapchains[swapchainSlot].swapchain->GetCurrentBackBufferIndex()};
}

// --- Command lists and submission ---------------------------------------------------------

Expected<std::unique_ptr<detail::CommandListBackend>, Error> D3D12Backend::createCommandList() {
    ComPtr<ID3D12Device4> device;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList7> list;
    if (const HRESULT result = m_device.As(&device); FAILED(result)) {
        return makeUnexpected(failure(result, "creating a command list"));
    }
    if (const HRESULT result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        FAILED(result)) {
        return makeUnexpected(failure(result, "creating a command allocator"));
    }
    // Created closed; begin() resets it.
    if (const HRESULT result = device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                          D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&list));
        FAILED(result)) {
        return makeUnexpected(failure(result, "creating a command list"));
    }
    return std::unique_ptr<detail::CommandListBackend>(
        std::make_unique<D3D12CommandList>(*this, std::move(allocator), std::move(list)));
}

void D3D12Backend::beginFrame(std::uint32_t /*frameSlot*/) {
    // Each command list resets its own allocator when it is begun (D3D12CommandList).
}

Error D3D12Backend::signalFailed(HRESULT result) {
    // Work was queued but its completion can't be tracked: nothing could tell when objects it
    // uses may be released, and waits for it would never end. The device is removed, so its
    // fence completes and the failure is the device loss every later call reports.
    removeDevice();
    Error error = lostError(m_device->GetDeviceRemovedReason());
    error.message += "; signaling the graphics timeline failed (" + hex(result) + ")";
    return error;
}

HRESULT D3D12Backend::signal(std::uint64_t value) {
    if (m_failNextSignal.exchange(false, std::memory_order_acq_rel)) {
        return E_FAIL;
    }
    return m_queue->Signal(m_fence.Get(), value);
}

Expected<void, Error> D3D12Backend::submit(std::span<detail::CommandListBackend* const> lists,
                                           std::uint64_t signalValue) {
    m_submitLists.clear();
    HRESULT closed = S_OK;
    for (detail::CommandListBackend* list : lists) {
        auto* d3d12List = static_cast<D3D12CommandList*>(list);
        if (const HRESULT result = d3d12List->close(); FAILED(result) && SUCCEEDED(closed)) {
            closed = result;
        }
        m_submitLists.push_back(d3d12List->native());
    }
    if (FAILED(closed)) {
        return makeUnexpected(failure(closed, "recording a command list"));
    }
    if (!m_submitLists.empty()) {
        m_queue->ExecuteCommandLists(static_cast<UINT>(m_submitLists.size()), m_submitLists.data());
    }
    if (const HRESULT signaled = signal(signalValue); FAILED(signaled)) {
        return makeUnexpected(signalFailed(signaled));
    }
    if (removed()) {
        return makeUnexpected(lostError(m_device->GetDeviceRemovedReason()));
    }
    return {};
}

Expected<detail::PresentStatus, Error> D3D12Backend::endFrame(std::uint32_t /*frameSlot*/,
                                                              std::optional<std::uint32_t> swapchainSlot,
                                                              std::uint64_t signalValue) {
#ifdef ITH_ENABLE_PROFILING
    // Once per frame: the zones of earlier frames the GPU has finished.
    m_gpuProfiler.collect();
#endif
    HRESULT presented = S_OK;
    const SwapchainRecord* presentedSwapchain = nullptr;
    if (swapchainSlot) {
        const SwapchainRecord& swapchain = m_swapchains[*swapchainSlot];
        presentedSwapchain = &swapchain;
        const bool tear = !swapchain.vsync && (swapchain.flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
        presented = swapchain.swapchain->Present(swapchain.vsync ? 1 : 0, tear ? DXGI_PRESENT_ALLOW_TEARING : 0);
    }
    // Signaled even when presenting failed (Backend.h).
    if (const HRESULT signaled = signal(signalValue); FAILED(signaled)) {
        return makeUnexpected(signalFailed(signaled));
    }
    if (removed()) {
        return makeUnexpected(lostError(m_device->GetDeviceRemovedReason()));
    }
    if (FAILED(presented) && presentedSwapchain != nullptr) {
        return makeUnexpected(failure(presented, "presenting swapchain '" + presentedSwapchain->name + "'"));
    }
    return detail::PresentStatus::Presented;
}

std::uint64_t D3D12Backend::completedValue() {
    // UINT64_MAX once the device is removed: everything counts as finished.
    return m_fence->GetCompletedValue();
}

Expected<void, Error> D3D12Backend::waitForValue(std::uint64_t value) {
    // Waits in steps of kWaitStep and looks for device removal after each, so that no wait is
    // without a limit, whatever a driver does with a removed device's fence.
    constexpr DWORD kWaitStep = 1000; // milliseconds
    if (m_fence->GetCompletedValue() < value) {
        if (const HRESULT result = m_fence->SetEventOnCompletion(value, m_fenceEvent.get()); FAILED(result)) {
            return makeUnexpected(failure(result, "waiting for the GPU"));
        }
        while (m_fence->GetCompletedValue() < value && !removed()) {
            if (WaitForSingleObject(m_fenceEvent.get(), kWaitStep) == WAIT_FAILED) {
                return makeUnexpected(failure(HRESULT_FROM_WIN32(GetLastError()), "waiting for the GPU"));
            }
        }
    }
    if (removed()) {
        return makeUnexpected(lostError(m_device->GetDeviceRemovedReason()));
    }
    return {};
}

} // namespace ith::rhi::d3d12

// --- Factory ------------------------------------------------------------------------------

namespace ith::rhi {
namespace {

// The backend of a Direct3D 12 device, or nullptr (asserted) for a moved-from device or another
// backend's.
d3d12::D3D12Backend* d3d12Backend(Device& device, [[maybe_unused]] const char* caller) {
    detail::DeviceBackend* backend = detail::backendOf(device);
    if (backend == nullptr || backend->capabilities().backend != Backend::D3D12) {
        ITH_ASSERT_MSG(false, caller);
        return nullptr;
    }
    return static_cast<d3d12::D3D12Backend*>(backend);
}

} // namespace

Expected<Device, Error> createD3D12Device(const DeviceDesc& desc) {
    Expected<std::unique_ptr<d3d12::D3D12Backend>, Error> backend = d3d12::D3D12Backend::create(desc);
    if (!backend) {
        return makeUnexpected(std::move(backend).error());
    }
    return detail::makeDevice(std::move(*backend));
}

void removeD3D12DeviceForTesting(Device& device) {
    if (d3d12::D3D12Backend* backend =
            d3d12Backend(device, "removeD3D12DeviceForTesting() needs a live Direct3D 12 device")) {
        backend->removeDevice();
    }
}

void failNextD3D12SignalForTesting(Device& device) {
    if (d3d12::D3D12Backend* backend =
            d3d12Backend(device, "failNextD3D12SignalForTesting() needs a live Direct3D 12 device")) {
        backend->failNextSignal();
    }
}

bool d3d12GpuZonesDisabledForTesting(Device& device) {
    const d3d12::D3D12Backend* backend =
        d3d12Backend(device, "d3d12GpuZonesDisabledForTesting() needs a live Direct3D 12 device");
#ifdef ITH_ENABLE_PROFILING
    return backend != nullptr && backend->gpuProfiler().disabled();
#else
    static_cast<void>(backend);
    return false;
#endif
}

} // namespace ith::rhi
