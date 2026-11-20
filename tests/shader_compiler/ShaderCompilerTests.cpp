#include "ith/shader_compiler/ShaderCompiler.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ostream> // doctest prints std::string_view through operator<<
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kShaderSource = R"(
struct VertexOutput {
    float4 position : SV_Position;
    float3 color : COLOR;
};

VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    const float2 corner = float2(float((vertexId << 1) & 2), float(vertexId & 2));
    VertexOutput output;
    output.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    output.color = float3(corner, 1.0);
    return output;
}

float4 pixelMain(VertexOutput input) : SV_Target {
    return float4(input.color, 1.0);
}

RWStructuredBuffer<uint> values;

[numthreads(64, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) {
    values[id.x] = id.x * 2;
}
)";

struct EntryPoint {
    std::string_view name;
    ith::ShaderStage stage;
};

constexpr std::array<EntryPoint, 3> kEntryPoints = {{
    {.name = "vertexMain", .stage = ith::ShaderStage::Vertex},
    {.name = "pixelMain", .stage = ith::ShaderStage::Pixel},
    {.name = "computeMain", .stage = ith::ShaderStage::Compute},
}};

ith::ShaderCompiler createCompiler() {
    ith::Expected<ith::ShaderCompiler> compiler = ith::ShaderCompiler::create();
    REQUIRE(compiler.hasValue());
    return std::move(*compiler);
}

std::uint32_t readWord(const std::vector<std::byte>& bytes, std::size_t offset) {
    std::uint32_t word = 0;
    std::memcpy(&word, bytes.data() + offset, sizeof(word));
    return word;
}

void checkSpirv(const std::vector<std::byte>& code) {
    REQUIRE(code.size() >= 20); // The SPIR-V header is five words.
    CHECK(code.size() % 4 == 0);
    CHECK(readWord(code, 0) == 0x07230203u); // The SPIR-V magic number, little-endian.
}

// A DXIL container header is "DXBC", a 16-byte digest, a 4-byte version and the 4-byte container
// size. DXC leaves the digest all zero when the code wasn't validated, and fills it with 0x02
// bytes for code that bypassed validation; only the validator writes a real digest.
void checkSignedDxil(const std::vector<std::byte>& code) {
    REQUIRE(code.size() > 32);
    CHECK(std::memcmp(code.data(), "DXBC", 4) == 0);
    CHECK(readWord(code, 24) == code.size());
    const auto digestBegin = code.begin() + 4;
    const auto digestEnd = digestBegin + 16;
    CHECK_FALSE(std::all_of(digestBegin, digestEnd, [](std::byte value) { return value == std::byte{0}; }));
    CHECK_FALSE(std::all_of(digestBegin, digestEnd, [](std::byte value) { return value == std::byte{2}; }));
}

ith::ShaderRequest request(std::string_view source, const EntryPoint& entryPoint, ith::ShaderTarget target) {
    return {
        .name = "Test.slang",
        .source = source,
        .entryPoint = entryPoint.name,
        .stage = entryPoint.stage,
        .target = target,
    };
}

void checkCompileFailure(const ith::Expected<std::vector<std::byte>>& result) {
    REQUIRE_FALSE(result.hasValue());
    CHECK(result.error().code == "shader_compiler.compile_failed");
    CHECK(result.error().message.find("Test.slang") != std::string::npos);
}

} // namespace

TEST_SUITE("shader_compiler") {
    TEST_CASE("every stage compiles to SPIR-V") {
        ith::ShaderCompiler compiler = createCompiler();
        for (const EntryPoint& entryPoint : kEntryPoints) {
            CAPTURE(entryPoint.name);
            const ith::Expected<std::vector<std::byte>> code =
                compiler.compile(request(kShaderSource, entryPoint, ith::ShaderTarget::Spirv));
            REQUIRE_MESSAGE(code.hasValue(), (code.hasValue() ? std::string() : code.error().message));
            checkSpirv(*code);
        }
    }

    TEST_CASE("every stage compiles to validated and signed DXIL") {
        ith::ShaderCompiler compiler = createCompiler();
        for (const EntryPoint& entryPoint : kEntryPoints) {
            CAPTURE(entryPoint.name);
            const ith::Expected<std::vector<std::byte>> code =
                compiler.compile(request(kShaderSource, entryPoint, ith::ShaderTarget::Dxil));
            REQUIRE_MESSAGE(code.hasValue(), (code.hasValue() ? std::string() : code.error().message));
            checkSignedDxil(*code);
        }
    }

    TEST_CASE("a syntax error fails with diagnostics that name the source and line") {
        ith::ShaderCompiler compiler = createCompiler();
        constexpr std::string_view kBroken = "\n\nfloat4 pixelMain() : SV_Target { return float4(1.0) }\n";
        for (const ith::ShaderTarget target : {ith::ShaderTarget::Spirv, ith::ShaderTarget::Dxil}) {
            const ith::Expected<std::vector<std::byte>> code =
                compiler.compile(request(kBroken, kEntryPoints[1], target));
            checkCompileFailure(code);
            CHECK_MESSAGE(code.error().message.find("Test.slang:3:") != std::string::npos, code.error().message);
        }
    }

    TEST_CASE("SPIR-V that fails validation is rejected") {
        // Inline SPIR-V assembly lets the source emit an instruction the validator rejects: an
        // integer add of a float.
        constexpr std::string_view kInvalid = "RWStructuredBuffer<float> values;\n"
                                              "[numthreads(1, 1, 1)] void computeMain() {\n"
                                              "    float a = values[1];\n"
                                              "    values[0] = spirv_asm { result:$$float = OpIAdd $a $a };\n"
                                              "}\n";
        ith::ShaderCompiler compiler = createCompiler();
        const ith::Expected<std::vector<std::byte>> code =
            compiler.compile(request(kInvalid, kEntryPoints[2], ith::ShaderTarget::Spirv));
        REQUIRE_FALSE_MESSAGE(code.hasValue(), "the invalid SPIR-V was accepted");
        CHECK(code.error().code == "shader_compiler.compile_failed");
        CHECK_MESSAGE(code.error().message.find("SPIR-V") != std::string::npos, code.error().message);
    }

    TEST_CASE("a missing entry point fails") {
        ith::ShaderCompiler compiler = createCompiler();
        const EntryPoint missing{.name = "noSuchFunction", .stage = ith::ShaderStage::Compute};
        checkCompileFailure(compiler.compile(request(kShaderSource, missing, ith::ShaderTarget::Spirv)));
    }

    TEST_CASE("empty source fails because it has no entry point") {
        ith::ShaderCompiler compiler = createCompiler();
        checkCompileFailure(compiler.compile(request("", kEntryPoints[2], ith::ShaderTarget::Spirv)));
    }

    TEST_CASE("a compiler keeps working after a failure, and a name can be compiled again with new source") {
        ith::ShaderCompiler compiler = createCompiler();
        const EntryPoint compute = kEntryPoints[2];
        constexpr std::string_view kDouble = "RWStructuredBuffer<uint> values;\n"
                                             "[numthreads(64, 1, 1)] void computeMain(uint3 id : SV_DispatchThreadID)"
                                             " { values[id.x] = id.x * 2; }\n";
        constexpr std::string_view kTriple = "RWStructuredBuffer<uint> values;\n"
                                             "[numthreads(64, 1, 1)] void computeMain(uint3 id : SV_DispatchThreadID)"
                                             " { values[id.x] = id.x * 3; }\n";

        const ith::Expected<std::vector<std::byte>> first =
            compiler.compile(request(kDouble, compute, ith::ShaderTarget::Spirv));
        REQUIRE(first.hasValue());
        checkCompileFailure(compiler.compile(request("not a shader", compute, ith::ShaderTarget::Spirv)));
        const ith::Expected<std::vector<std::byte>> second =
            compiler.compile(request(kTriple, compute, ith::ShaderTarget::Spirv));
        REQUIRE(second.hasValue());
        CHECK(*first != *second);

        const ith::Expected<std::vector<std::byte>> again =
            compiler.compile(request(kDouble, compute, ith::ShaderTarget::Spirv));
        REQUIRE(again.hasValue());
        CHECK(*again == *first);
    }

    TEST_CASE("a moved compiler keeps compiling") {
        ith::ShaderCompiler original = createCompiler();
        ith::ShaderCompiler moved = std::move(original);
        const ith::Expected<std::vector<std::byte>> code =
            moved.compile(request(kShaderSource, kEntryPoints[2], ith::ShaderTarget::Spirv));
        REQUIRE(code.hasValue());
        checkSpirv(*code);
    }
}
