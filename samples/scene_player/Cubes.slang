// The instanced cubes of scene_player. One indexed, instanced draw: the vertex shader reads the
// cube's vertices and the instance's placement and color from bindless buffers whose read indices
// are push constants, and the pixel shader lights each face by one directional light, so the
// faces of a cube read apart.
//
// One source serves both APIs. ith_embed_shaders defines ITH_SPIRV when it compiles SPIR-V, which
// reaches buffers through the RHI's bindless descriptor set; DXIL indexes the descriptor heap.

struct Constants {
    // World space to clip space, column-major (ADR-0006).
    float4x4 viewProjection;
    // The read index of the cube's 24 vertices of 24 bytes: float3 position, float3 normal.
    uint vertices;
    // The read index of the instances, 32 bytes each: float3 position, float scale, float4 color.
    uint instances;
};
[[vk::push_constant]] ConstantBuffer<Constants> constants;

#ifdef ITH_SPIRV
[[vk::binding(0, 0)]] ByteAddressBuffer buffers[];
ByteAddressBuffer buffer(uint index) {
    return buffers[index];
}
#else
ByteAddressBuffer buffer(uint index) {
    return ResourceDescriptorHeap[index];
}
#endif

struct Varyings {
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float3 color : COLOR;
};

Varyings cubeVertex(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    const ByteAddressBuffer vertices = buffer(constants.vertices);
    const ByteAddressBuffer instances = buffer(constants.instances);
    const float4 placement = instances.Load<float4>(instanceId * 32);
    const float3 corner = vertices.Load<float3>(vertexId * 24);
    Varyings output;
    output.position = mul(constants.viewProjection, float4(placement.xyz + corner * placement.w, 1.0));
    output.normal = vertices.Load<float3>(vertexId * 24 + 12);
    output.color = instances.Load<float4>(instanceId * 32 + 16).rgb;
    return output;
}

// An ambient term plus Lambert from a light above, to the right and in front (+X, +Y, +Z).
float4 cubePixel(Varyings input) : SV_Target {
    const float3 toLight = normalize(float3(0.3, 0.8, 0.5));
    const float shade = 0.25 + 0.75 * saturate(dot(normalize(input.normal), toLight));
    return float4(input.color * shade, 1.0);
}
