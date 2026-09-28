// Dear ImGui renderer shaders for wvk.
//
// These are compiled ahead of time into imgui_impl_wvk_spirv.inl, so the backend needs no shader
// compiler at run time. After editing this file, regenerate the header from the wvk root with:
//
//     xmake lua imgui/generate_spirv.lua
//
// The flags match xmake/rules/hlsl2spv.lua (scalar layout, resource heap at set 0 binding 0,
// sampler heap at set 0 binding 1), so this file can also be compiled by that rule and handed to
// ImGui_ImplWVK_InitInfo::VertexSpirv / FragmentSpirv as a starting point for a custom variant.

#include "shaders/wvk.hlsl"

// Must match ImDrawVert: 20 bytes under scalar layout.
struct ImGuiVertex
{
    float2 position;
    float2 uv;
    uint   color; // RGBA8, R in the low byte.
};

// Must match ImGui_ImplWVK_Root in imgui_impl_wvk.cpp.
struct ImGuiRoot
{
    float2         scale;
    float2         translate;
    uint           vertex_offset; // ImDrawCmd::VtxOffset plus the draw list's base vertex.
    ResourceHandle vertices;
    ResourceHandle texture;
    ResourceHandle texture_sampler;
    uint           flags;
};

// Set when the colour target is an sRGB format. ImGui colours are authored in sRGB space, so they
// are linearized here and the hardware re-encodes them on write; without this the UI washes out.
static const uint IMGUI_WVK_FLAG_SRGB_TARGET = 1u;

WVK_PUSH_CONSTANTS(ImGuiRoot, g_root);

struct Varyings
{
    float4 position : SV_Position;
    float4 color    : COLOR0;
    float2 uv       : TEXCOORD0;
};

float3 srgb_to_linear(float3 c)
{
    const float3 low  = c / 12.92f;
    const float3 high = pow((c + 0.055f) / 1.055f, 2.4f);
    return select(c <= 0.04045f, low, high);
}

Varyings vertex_main(uint vertex_id : SV_VertexID)
{
    WVKStructuredBuffer<ImGuiVertex> vertices = WVKStructuredBuffer<ImGuiVertex>::Create(g_root.vertices);
    const ImGuiVertex v = vertices.Load(vertex_id + g_root.vertex_offset);

    float4 color = float4(
        float((v.color      ) & 0xffu),
        float((v.color >>  8) & 0xffu),
        float((v.color >> 16) & 0xffu),
        float((v.color >> 24) & 0xffu)) / 255.0f;
    if (g_root.flags & IMGUI_WVK_FLAG_SRGB_TARGET)
        color.rgb = srgb_to_linear(color.rgb);

    Varyings output;
    output.position = float4(v.position * g_root.scale + g_root.translate, 0.0f, 1.0f);
    output.color    = color;
    output.uv       = v.uv;
    return output;
}

float4 pixel_main(Varyings input) : SV_Target
{
    WVKTexture2D<float4> tex = WVKTexture2D<float4>::Create(g_root.texture);
    WVKSampler           smp = WVKSampler::Create(g_root.texture_sampler);
    return input.color * tex.Sample(smp, input.uv);
}
