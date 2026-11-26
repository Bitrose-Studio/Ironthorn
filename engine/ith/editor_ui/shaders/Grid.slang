// The viewport's grid pass (ADR-0042 rules 15 and 18): an infinite grid on the plane y = 0, drawn
// as one fullscreen triangle. Each pixel casts its view ray at the plane and writes the hit's depth,
// so the grid is depth-tested against the shapes (reversed depth, ADR-0006 rule 6). Minor lines
// every metre, major lines every ten, the world X axis (z = 0) and Z axis (x = 0) in their own
// colours; everything fades with distance, and the minor lines also fade out where they would be
// closer together than a few pixels. Lines are about one pixel wide, antialiased with screen-space
// derivatives.
//
// Colours are four 8-bit sRGB channels, red in the lowest byte, as the theme gives them; they are
// decoded to linear and blended with straight alpha over the shapes.

struct Constants {
    // xyz: the camera's right axis times tan(horizontal fov / 2); w: the near plane in metres.
    float4 right;
    // xyz: the camera's up axis times tan(vertical fov / 2); w: the viewport's width in pixels.
    float4 up;
    // xyz: the camera's forward axis (unit length); w: the viewport's height in pixels.
    float4 forward;
    // xyz: the camera's world position; w: the distance in metres at which the grid has faded out.
    float4 camera;
    // The minor line, major line, X axis and Z axis colours.
    uint4 colors;
};
[[vk::push_constant]] ConstantBuffer<Constants> constants;

float4 gridVertex(uint vertexId : SV_VertexID) : SV_Position {
    // A triangle covering the viewport: (-1, -1), (3, -1), (-1, 3).
    const float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

// The sRGB transfer function, inverted: an encoded channel in 0..1 to linear.
float3 srgbToLinear(float3 encoded) {
    const float3 low = encoded / 12.92;
    const float3 high = pow((encoded + 0.055) / 1.055, 2.4);
    return select(encoded <= 0.04045, low, high);
}

float4 unpackColor(uint packed) {
    const float4 color = float4(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF, packed >> 24) / 255.0;
    return float4(srgbToLinear(color.rgb), color.a);
}

// How much of a line every `spacing` metres covers the pixel at `coord` (world x and z).
float lineCoverage(float2 coord, float spacing) {
    const float2 cells = coord / spacing;
    const float2 width = max(fwidth(cells), 1e-6);
    const float2 distance = abs(frac(cells - 0.5) - 0.5) / width;
    return 1.0 - saturate(min(distance.x, distance.y));
}

// How much of the line at value 0 of `value` (a world coordinate) covers the pixel.
float axisCoverage(float value) {
    return 1.0 - saturate(abs(value) / max(fwidth(value), 1e-6));
}

// Straight-alpha `top` over `bottom`.
float4 over(float4 top, float4 bottom) {
    const float alpha = top.a + bottom.a * (1.0 - top.a);
    if (alpha <= 0.0) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return float4((top.rgb * top.a + bottom.rgb * bottom.a * (1.0 - top.a)) / alpha, alpha);
}

float4 gridPixel(float4 position : SV_Position, out float depth : SV_Depth) : SV_Target {
    const float2 ndc =
        float2(position.x / constants.up.w * 2.0 - 1.0, 1.0 - position.y / constants.forward.w * 2.0);
    // The ray's direction, scaled so it advances one metre along the view axis per unit.
    const float3 ray = constants.forward.xyz + ndc.x * constants.right.xyz + ndc.y * constants.up.xyz;
    const float height = constants.camera.y;
    // Whether the ray meets the plane in front of the camera. Pixels that miss are discarded at the
    // end, after the derivatives, which need every pixel of a quad.
    const bool hits = ray.y * height < 0.0;
    const float along = hits ? -height / ray.y : 1.0;
    const float3 hit = ray * along;
    // Reversed infinite projection: depth is near / view distance.
    depth = constants.right.w / along;
    const float2 world = constants.camera.xz + hit.xz;

    const float minorFade = saturate(1.5 - max(fwidth(world.x), fwidth(world.y)) * 6.0);
    float4 color = unpackColor(constants.colors.x);
    color.a *= lineCoverage(world, 1.0) * minorFade;
    float4 major = unpackColor(constants.colors.y);
    major.a *= lineCoverage(world, 10.0);
    color = over(major, color);
    float4 axisX = unpackColor(constants.colors.z);
    axisX.a *= axisCoverage(world.y);
    color = over(axisX, color);
    float4 axisZ = unpackColor(constants.colors.w);
    axisZ.a *= axisCoverage(world.x);
    color = over(axisZ, color);
    color.a *= 1.0 - saturate(length(hit.xz) / constants.camera.w);
    if (!hits) {
        discard;
    }
    return color;
}
