// The viewport's grid pass (ADR-0042 rules 15 and 18): an infinite grid on the plane y = 0, drawn
// as one fullscreen triangle. Each pixel casts its view ray at the plane and writes the hit's depth,
// so the grid is depth-tested against the shapes (reversed depth, ADR-0006 rule 6). Minor lines
// every metre, major lines every ten, the world X axis (z = 0) and Z axis (x = 0) in their own
// colours.
//
// Every line is about one pixel wide on screen whatever its distance, antialiased with the screen
// gradient of the world coordinates. Seen far away or at a grazing angle, parallel lines come closer
// together on screen than a pixel, where they would alias into false curves and moire; so each level
// fades out as its cells shrink on screen toward a few pixels across, minor lines first, then major
// lines. The axes are single lines and cannot alias. Everything also fades out with distance,
// smoothly to zero, so the grid has no visible edge.
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

// A level of lines is drawn fully while its lines are at least this many pixels apart on screen in
// both directions, and not at all once they are this close or closer in either; it fades linearly in
// between.
static const float kFullSpacingPixels = 10.0;
static const float kGoneSpacingPixels = 3.0;

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

// The change of each component of `value` per pixel: the length of its screen gradient,
// so a line of constant value is measured across, not along a pixel axis. Never zero.
float2 perPixel(float2 value) {
    const float2 x = ddx(value);
    const float2 y = ddy(value);
    return max(sqrt(x * x + y * y), 1e-6);
}

// How much of a line about one pixel wide, where `value` is 0, covers the pixel; `step` is the
// change of `value` per pixel (perPixel).
float lineCoverage(float value, float step) {
    return 1.0 - saturate(abs(value) / step);
}

// How much the lines every `spacing` metres cover the pixel at `coord` (world x and z). The level
// fades out where its cells get smaller on screen than a few pixels across in either direction, so
// neither family of its lines ever comes close enough together to alias.
float gridCoverage(float2 coord, float spacing) {
    const float2 cells = coord / spacing;
    const float2 step = perPixel(cells);
    // The signed distance to the nearest line, in cells.
    const float2 offset = frac(cells + 0.5) - 0.5;
    // The denser family's lines are 1 / step pixels apart on screen.
    const float closest = 1.0 / max(step.x, step.y);
    const float fade = saturate((closest - kGoneSpacingPixels) / (kFullSpacingPixels - kGoneSpacingPixels));
    return max(lineCoverage(offset.x, step.x), lineCoverage(offset.y, step.y)) * fade;
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

    float4 color = unpackColor(constants.colors.x);
    color.a *= gridCoverage(world, 1.0);
    float4 major = unpackColor(constants.colors.y);
    major.a *= gridCoverage(world, 10.0);
    color = over(major, color);
    // The axes are single lines, which cannot alias: they only fade with distance.
    const float2 axisStep = perPixel(world);
    float4 axisX = unpackColor(constants.colors.z);
    axisX.a *= lineCoverage(world.y, axisStep.y);
    color = over(axisX, color);
    float4 axisZ = unpackColor(constants.colors.w);
    axisZ.a *= lineCoverage(world.x, axisStep.x);
    color = over(axisZ, color);
    // A smooth fade to zero, with no kink where it ends, so no rim shows where the grid runs out.
    color.a *= 1.0 - smoothstep(0.0, 1.0, length(hit.xz) / constants.camera.w);
    if (!hits) {
        discard;
    }
    return color;
}
