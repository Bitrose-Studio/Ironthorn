// The viewport's shapes pass (ADR-0042 rules 15, 19 and 20): one draw per ShapeRenderer, unlit,
// with a fixed light from the camera so the shapes read as 3D. Rendering is camera-relative
// (ADR-0006 rule 7): the CPU subtracts the camera's position before it builds the matrices.
//
// A vertex is 24 bytes, pulled from a bindless buffer: float3 position and float3 normal, in the
// shape's local space. The index buffer holds each shape's indices already offset to its first
// vertex, so draws need no vertex offset and no API's rule for SV_VertexID matters.
//
// One source serves both APIs: SPIR-V compilations define ITH_SPIRV and reach the buffer through
// the RHI's bindless descriptor set; DXIL indexes the descriptor heap.

struct Constants {
    // Local position to clip space, camera-relative, as the rows of the matrix.
    float4 clip0;
    float4 clip1;
    float4 clip2;
    float4 clip3;
    // The local-to-view 3x3 (rotation and scale), as rows; w unused. Normals go through its
    // cofactor matrix, which is right for non-uniform scales too.
    float4 view0;
    float4 view1;
    float4 view2;
    // The linear colour, the selection tint already applied.
    float3 color;
    // The read index of the vertex buffer.
    uint vertices;
};
[[vk::push_constant]] ConstantBuffer<Constants> constants;

#ifdef ITH_SPIRV
[[vk::binding(0, 0)]] ByteAddressBuffer buffers[];
ByteAddressBuffer vertexBuffer() {
    return buffers[constants.vertices];
}
#else
ByteAddressBuffer vertexBuffer() {
    return ResourceDescriptorHeap[constants.vertices];
}
#endif

struct Varyings {
    float4 position : SV_Position;
    float3 normal : NORMAL0;
};

Varyings shapesVertex(uint vertexId : SV_VertexID) {
    const ByteAddressBuffer vertices = vertexBuffer();
    const uint offset = vertexId * 24;
    const float4 local = float4(vertices.Load<float3>(offset), 1.0);
    const float3 normal = vertices.Load<float3>(offset + 12);
    const float3 r0 = constants.view0.xyz;
    const float3 r1 = constants.view1.xyz;
    const float3 r2 = constants.view2.xyz;
    // The cofactor matrix is the inverse transpose times the determinant; its sign keeps mirrored
    // shapes' normals pointing out.
    const float3 c0 = cross(r1, r2);
    const float3 c1 = cross(r2, r0);
    const float3 c2 = cross(r0, r1);
    const float side = dot(r0, c0) < 0.0 ? -1.0 : 1.0;
    Varyings output;
    output.position = float4(dot(constants.clip0, local), dot(constants.clip1, local), dot(constants.clip2, local),
                             dot(constants.clip3, local));
    output.normal = float3(dot(c0, normal), dot(c1, normal), dot(c2, normal)) * side;
    return output;
}

// Towards the light, in view space (+Z points at the viewer): from the camera, a little above and
// to the left, so the faces of a box differ.
static const float3 kLight = float3(-0.3, 0.5, 0.81);

float4 shapesPixel(Varyings input) : SV_Target {
    const float3 normal = normalize(input.normal);
    const float shade = 0.3 + 0.7 * saturate(dot(normal, normalize(kLight)));
    return float4(constants.color * shade, 1.0);
}
