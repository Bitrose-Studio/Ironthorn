// The triangle of scene_player: three vertices read from a bindless buffer whose read index is
// the push constant, so the pipeline has no vertex input layout (ADR-0009), with the vertex
// colors interpolated across the triangle.
//
// One source serves both APIs. ith_embed_shaders defines ITH_SPIRV when it compiles SPIR-V, which
// reaches buffers through the RHI's bindless descriptor set; DXIL indexes the descriptor heap.

struct Constants {
    // The read index of 3 vertices of 32 bytes: float4 position (clip space), float4 color.
    uint vertices;
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
    float4 color : COLOR;
};

Varyings triangleVertex(uint vertexId : SV_VertexID) {
    const ByteAddressBuffer vertices = buffer(constants.vertices);
    Varyings output;
    output.position = vertices.Load<float4>(vertexId * 32);
    output.color = vertices.Load<float4>(vertexId * 32 + 16);
    return output;
}

float4 trianglePixel(Varyings input) : SV_Target {
    return input.color;
}
