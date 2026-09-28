#include "shaders/wvk.hlsl"

// The scene drawn under the UI: one spinning triangle whose parameters come from ImGui widgets.
// No vertex buffer at all; the corners are generated from SV_VertexID.

// Matches SceneRoot in main.cpp. Four scalars then a float4, so every layout rule agrees on the
// offsets (0, 4, 8, 12, 16).
struct SceneRoot
{
    float  aspect; // height / width, keeps the triangle equilateral in any window.
    float  angle;  // radians
    float  size;
    float  padding;
    float4 tint;
};

WVK_PUSH_CONSTANTS(SceneRoot, g_root);

static const float2 k_corners[3] = {
    float2( 0.0f,    0.577f),
    float2( 0.5f,   -0.289f),
    float2(-0.5f,   -0.289f),
};

static const float3 k_colors[3] = {
    float3(1.0f, 0.25f, 0.25f),
    float3(0.25f, 1.0f, 0.25f),
    float3(0.25f, 0.25f, 1.0f),
};

struct Varyings
{
    float4 position : SV_Position;
    float3 color    : COLOR0;
};

Varyings vertex_main(uint vertex_id : SV_VertexID)
{
    float s, c;
    sincos(g_root.angle, s, c);
    float2 p = k_corners[vertex_id] * g_root.size;
    p = float2(p.x * c - p.y * s, p.x * s + p.y * c);
    p.x *= g_root.aspect;

    Varyings output;
    output.position = float4(p, 0.0f, 1.0f);
    output.color    = k_colors[vertex_id];
    return output;
}

float4 pixel_main(Varyings input) : SV_Target
{
    return float4(input.color, 1.0f) * g_root.tint;
}
