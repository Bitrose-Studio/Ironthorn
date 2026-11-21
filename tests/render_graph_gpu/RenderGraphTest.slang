// Shaders of the render graph's GPU test: a fullscreen triangle at a given depth, a constant
// color, a weighted sum of two textures, and a tonemap. Textures are read by bindless sampled
// index with Load, one texel per pixel.
//
// One source serves both APIs: ith_embed_shaders defines ITH_SPIRV when it compiles SPIR-V, which
// reaches textures through the RHI's bindless descriptor set; DXIL indexes the descriptor heap.

struct Constants {
    float4 color;
    // The triangle's depth in clip space; depth is reversed, nearer is greater.
    float depth;
    // Sampled indices and weights of combinePixel; tonemapPixel reads inputA.
    uint inputA;
    uint inputB;
    float weightA;
    float weightB;
};
[[vk::push_constant]] ConstantBuffer<Constants> constants;

#ifdef ITH_SPIRV
[[vk::binding(1, 0)]] Texture2D textures[];
float4 loadTexel(uint index, float4 position) {
    return textures[index].Load(int3(int2(position.xy), 0));
}
#else
float4 loadTexel(uint index, float4 position) {
    Texture2D texture = ResourceDescriptorHeap[index];
    return texture.Load(int3(int2(position.xy), 0));
}
#endif

// Covers the target with one triangle: (-1, -1), (3, -1), (-1, 3), counter-clockwise.
float4 fullscreenVertex(uint vertexId : SV_VertexID) : SV_Position {
    const float2 corner = float2(float((vertexId << 1) & 2), float(vertexId & 2));
    return float4(corner * 2.0 - 1.0, constants.depth, 1.0);
}

float4 fillPixel(float4 position : SV_Position) : SV_Target {
    return constants.color;
}

float4 combinePixel(float4 position : SV_Position) : SV_Target {
    return loadTexel(constants.inputA, position) * constants.weightA +
           loadTexel(constants.inputB, position) * constants.weightB;
}

// Reinhard: c / (1 + c), opaque.
float4 tonemapPixel(float4 position : SV_Position) : SV_Target {
    const float3 color = loadTexel(constants.inputA, position).rgb;
    return float4(color / (1.0 + color), 1.0);
}
