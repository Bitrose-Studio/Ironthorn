#include "ith/shader_compiler/ShaderCompiler.h"

#include "ith/foundation/Assert.h"
#include "ith/foundation/StringUtils.h"

#include <slang-com-ptr.h>
#include <slang.h>

#include <array>
#include <cstring>
#include <string>
#include <utility>

namespace ith {
namespace {

SlangStage toSlangStage(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex:
            return SLANG_STAGE_VERTEX;
        case ShaderStage::Pixel:
            return SLANG_STAGE_FRAGMENT;
        case ShaderStage::Compute:
            return SLANG_STAGE_COMPUTE;
    }
    return SLANG_STAGE_NONE;
}

std::string_view blobText(slang::IBlob* blob) {
    if (blob == nullptr) {
        return {};
    }
    return trim(std::string_view(static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize()));
}

// The error for a failed step: Slang's diagnostics, or `fallback` when Slang printed none.
Unexpected<Error> compileError(slang::IBlob* diagnostics, std::string fallback) {
    const std::string_view text = blobText(diagnostics);
    return makeError("shader_compiler.compile_failed", text.empty() ? std::move(fallback) : std::string(text));
}

} // namespace

struct ShaderCompiler::Impl {
    Slang::ComPtr<slang::IGlobalSession> globalSession;
};

ShaderCompiler::ShaderCompiler(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

ShaderCompiler::ShaderCompiler(ShaderCompiler&& other) noexcept = default;
ShaderCompiler& ShaderCompiler::operator=(ShaderCompiler&& other) noexcept = default;
ShaderCompiler::~ShaderCompiler() = default;

Expected<ShaderCompiler> ShaderCompiler::create() {
    auto impl = std::make_unique<Impl>();
    if (SLANG_FAILED(slang::createGlobalSession(impl->globalSession.writeRef())) || !impl->globalSession) {
        return makeError("shader_compiler.unavailable", "the Slang shader compiler library could not be initialized");
    }
    return ShaderCompiler(std::move(impl));
}

Expected<std::vector<std::byte>> ShaderCompiler::compile(const ShaderRequest& request) {
    ITH_ASSERT_MSG(m_impl != nullptr, "ShaderCompiler used after it was moved from");
    slang::IGlobalSession& globalSession = *m_impl->globalSession;

    // A session per compilation: a session keeps every module it loaded, by name, so a fresh one
    // lets the same name be compiled again after its source changed.
    const bool dxil = request.target == ShaderTarget::Dxil;
    slang::TargetDesc target;
    target.format = dxil ? SLANG_DXIL : SLANG_SPIRV;
    target.profile = globalSession.findProfile(dxil ? "sm_6_6" : "spirv_1_6");
    ITH_ASSERT_MSG(target.profile != SLANG_PROFILE_UNKNOWN, "this Slang release doesn't know the engine's profile");

    // SPIR-V is checked by the SPIR-V validator in Slang's glslang library; DXIL is validated and
    // signed by DXC as part of code generation. Slang only warns when it can't load that library,
    // so the tests include SPIR-V that must fail validation.
    slang::CompilerOptionEntry validateSpirv;
    validateSpirv.name = slang::CompilerOptionName::ValidateWithDownstreamCompiler;
    validateSpirv.value.intValue0 = 1;

    slang::SessionDesc sessionDesc;
    sessionDesc.targets = &target;
    sessionDesc.targetCount = 1;
    sessionDesc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
    if (!dxil) {
        sessionDesc.compilerOptionEntries = &validateSpirv;
        sessionDesc.compilerOptionEntryCount = 1;
    }

    Slang::ComPtr<slang::ISession> session;
    if (SLANG_FAILED(globalSession.createSession(sessionDesc, session.writeRef())) || !session) {
        return makeError("shader_compiler.unavailable", "could not create a Slang compilation session");
    }

    // Slang reads C strings, while the request holds views.
    const std::string name(request.name);
    const std::string source(request.source);
    const std::string entryPointName(request.entryPoint);

    Slang::ComPtr<slang::IBlob> diagnostics;
    slang::IModule* module =
        session->loadModuleFromSourceString(name.c_str(), name.c_str(), source.c_str(), diagnostics.writeRef());
    if (module == nullptr) {
        return compileError(diagnostics, name + ": the source could not be compiled");
    }

    Slang::ComPtr<slang::IEntryPoint> entryPoint;
    diagnostics = nullptr;
    if (SLANG_FAILED(module->findAndCheckEntryPoint(entryPointName.c_str(), toSlangStage(request.stage),
                                                    entryPoint.writeRef(), diagnostics.writeRef())) ||
        !entryPoint) {
        return compileError(diagnostics,
                            name + ": entry point '" + entryPointName + "' was not found or is invalid for its stage");
    }

    const std::array<slang::IComponentType*, 2> components = {module, entryPoint.get()};
    Slang::ComPtr<slang::IComponentType> program;
    diagnostics = nullptr;
    if (SLANG_FAILED(session->createCompositeComponentType(components.data(), static_cast<SlangInt>(components.size()),
                                                           program.writeRef(), diagnostics.writeRef()))) {
        return compileError(diagnostics, name + ": the program could not be composed");
    }

    Slang::ComPtr<slang::IComponentType> linked;
    diagnostics = nullptr;
    if (SLANG_FAILED(program->link(linked.writeRef(), diagnostics.writeRef()))) {
        return compileError(diagnostics, name + ": the program could not be linked");
    }

    Slang::ComPtr<slang::IBlob> code;
    diagnostics = nullptr;
    if (SLANG_FAILED(linked->getEntryPointCode(0, 0, code.writeRef(), diagnostics.writeRef())) || !code ||
        code->getBufferSize() == 0) {
        return compileError(diagnostics, name + ": code generation failed");
    }

    std::vector<std::byte> bytecode(code->getBufferSize());
    std::memcpy(bytecode.data(), code->getBufferPointer(), bytecode.size());
    return bytecode;
}

} // namespace ith
