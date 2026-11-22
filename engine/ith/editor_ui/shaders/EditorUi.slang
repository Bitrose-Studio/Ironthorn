// The editor UI's one pipeline (ADR-0029 rules 9 and 11): Dear ImGui's vertices pulled from a
// bindless buffer, each draw's texture sampled through a bindless index with a linear clamp
// sampler, and vertex colours decoded from sRGB to linear, so blending happens in linear space and
// the sRGB target encodes on write.
//
// A vertex is 20 bytes: float2 position in display coordinates, float2 texture coordinates, and a
// colour of four 8-bit sRGB channels, red in the lowest byte. The draw's first vertex comes in the
// constants, not from the API's vertex offset, so no API's rule for SV_VertexID matters.
//
// One source serves both APIs: SPIR-V compilations define ITH_SPIRV and reach resources through
// the RHI's bindless descriptor set; DXIL indexes the descriptor heaps.

struct Constants {
    // Display coordinates to clip space: clip = position * scale + translate.
    float2 scale;
    float2 translate;
    // The read index of the vertex buffer, the sampled index of the texture, the sampler index.
    uint vertices;
    uint texture;
    uint textureSampler;
    // The draw's first vertex in the buffer.
    uint vertexOffset;
};
[[vk::push_constant]] ConstantBuffer<Constants> constants;

#ifdef ITH_SPIRV
[[vk::binding(0, 0)]] ByteAddressBuffer buffers[];
[[vk::binding(1, 0)]] Texture2D textures[];
[[vk::binding(3, 0)]] SamplerState samplers[];
ByteAddressBuffer vertexBuffer() {
    return buffers[constants.vertices];
}
float4 sampleTexture(float2 uv) {
    return textures[constants.texture].Sample(samplers[constants.textureSampler], uv);
}
#else
ByteAddressBuffer vertexBuffer() {
    return ResourceDescriptorHeap[constants.vertices];
}
float4 sampleTexture(float2 uv) {
    Texture2D texture = ResourceDescriptorHeap[constants.texture];
    SamplerState textureSampler = SamplerDescriptorHeap[constants.textureSampler];
    return texture.Sample(textureSampler, uv);
}
#endif

struct Varyings {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

// The sRGB transfer function, inverted: an encoded channel in 0..1 to linear.
float3 srgbToLinear(float3 encoded) {
    const float3 low = encoded / 12.92;
    const float3 high = pow((encoded + 0.055) / 1.055, 2.4);
    return select(encoded <= 0.04045, low, high);
}

Varyings editorUiVertex(uint vertexId : SV_VertexID) {
    const ByteAddressBuffer vertices = vertexBuffer();
    const uint offset = (constants.vertexOffset + vertexId) * 20;
    const float2 position = vertices.Load<float2>(offset);
    const uint packed = vertices.Load(offset + 16);
    const float4 color = float4(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF, packed >> 24) / 255.0;
    Varyings output;
    output.position = float4(position * constants.scale + constants.translate, 0.0, 1.0);
    output.uv = vertices.Load<float2>(offset + 8);
    output.color = float4(srgbToLinear(color.rgb), color.a);
    return output;
}

float4 editorUiPixel(Varyings input) : SV_Target {
    return input.color * sampleTexture(input.uv);
}
