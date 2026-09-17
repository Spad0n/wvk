// "%VULKAN_SDK%\Bin\dxc.exe" -spirv -T vs_6_6 -E vertex_main -fspv-entrypoint-name=main -fvk-use-scalar-layout -fspv-target-env=vulkan1.3 -fvk-bind-resource-heap 0 0 -fvk-bind-sampler-heap 1 0 -Fo triangle.vert.spv triangle.hlsl
// "%VULKAN_SDK%\Bin\dxc.exe" -spirv -T ps_6_6 -E pixel_main -fspv-entrypoint-name=main -fvk-use-scalar-layout -fspv-target-env=vulkan1.3 -fvk-bind-resource-heap 0 0 -fvk-bind-sampler-heap 1 0 -Fo triangle.frag.spv triangle.hlsl
#include "shaders/wvk.hlsl"

// Matches Vertex in triangle.cpp. Both members are float4 on purpose: a float3 sits at offset 12
// under DX packing but at offset 16 under std430, so the two layouts disagree on a struct of two
// float3s and the mismatch only shows up as garbled geometry. Padding to float4 makes every layout
// rule produce the same 32 bytes. If you would rather pack tightly, compile with
// -fvk-use-scalar-layout and match it on the C++ side.
struct Vertex
{
    float3 position;
    float3 color;
};

// The whole root signature: one bindless handle and one constant. wvk binds no descriptor sets per
// draw, so anything the shader needs travels through here.
struct TriangleRoot
{
    ResourceHandle vertices;
};

WVK_PUSH_CONSTANTS(TriangleRoot, g_root);

struct Varyings
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

// No vertex input. The pipeline has an empty VkPipelineVertexInputStateCreateInfo, so the shader
// fetches its own data out of the descriptor heap using SV_VertexID.
Varyings vertex_main(uint vertex_id : SV_VertexID)
{
    WVKStructuredBuffer<Vertex> vertices = WVKStructuredBuffer<Vertex>::Create(g_root.vertices);
    const Vertex v = vertices.Load(vertex_id);

    Varyings output;
    //output.position = float4(v.position.xy * g_root.scale, v.position.z, 1.0f);
    output.position = float4(v.position, 1.0f);
    output.color = v.color;
    return output;
}

float4 pixel_main(Varyings input) : SV_Target
{
    return float4(input.color, 1.0f);
}
