// "%VULKAN_SDK%\Bin\dxc.exe" -spirv -T vs_6_6 -E vertex_main -fspv-entrypoint-name=main -fvk-use-scalar-layout -fspv-target-env=vulkan1.3 -fvk-bind-resource-heap 0 0 -fvk-bind-sampler-heap 1 0 -Fo quad.vert.spv quad.hlsl
// "%VULKAN_SDK%\Bin\dxc.exe" -spirv -T ps_6_6 -E pixel_main -fspv-entrypoint-name=main -fvk-use-scalar-layout -fspv-target-env=vulkan1.3 -fvk-bind-resource-heap 0 0 -fvk-bind-sampler-heap 1 0 -Fo quad.frag.spv quad.hlsl
#include "shaders/wvk.hlsl"

// Doit correspondre a Vertex dans quad.cpp (16 octets, scalar layout).
struct Vertex
{
    float2 position;
    float2 uv;
};

// Trois handles bindless : le buffer de sommets, la texture et le sampler.
// Attention : "texture" et "sampler" sont des mots-cles HLSL, d'ou ces noms.
struct QuadRoot
{
    ResourceHandle vertices;
    ResourceHandle albedo;         // slot dans ResourceDescriptorHeap
    ResourceHandle albedo_sampler; // slot dans SamplerDescriptorHeap
};

WVK_PUSH_CONSTANTS(QuadRoot, g_root);

struct Varyings
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

Varyings vertex_main(uint vertex_id : SV_VertexID)
{
    WVKStructuredBuffer<Vertex> vertices = WVKStructuredBuffer<Vertex>::Create(g_root.vertices);
    const Vertex v = vertices.Load(vertex_id);

    Varyings output;
    output.position = float4(v.position, 0.0f, 1.0f);
    output.uv       = v.uv;
    return output;
}

float4 pixel_main(Varyings input) : SV_Target
{
    WVKTexture2D<float4> albedo = WVKTexture2D<float4>::Create(g_root.albedo);
    WVKSampler           smp    = WVKSampler::Create(g_root.albedo_sampler);
    return albedo.Sample(smp, input.uv);
}
