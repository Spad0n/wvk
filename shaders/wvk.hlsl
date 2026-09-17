#ifndef WVK_SHADER_HLSL
#define WVK_SHADER_HLSL

static const int WVK_INVALID_DESCRIPTOR = -1;
typedef uint ResourceHandle;

#define WVK_PUSH_CONSTANTS(type, name) [[vk::push_constant]] ConstantBuffer<type> name : register(b0)

class WVKSampler
{
    ResourceHandle handle;
    SamplerState state;

    static WVKSampler Create(ResourceHandle id)
    {
        WVKSampler s;
        s.handle = id;
        s.state  = SamplerDescriptorHeap[id];
        return s;
    }

    ResourceHandle Handle() { return handle; }
    SamplerState   Resource() { return state; }
};

class WVKComparisonSampler
{
    ResourceHandle handle;
    SamplerComparisonState state;

    static WVKComparisonSampler Create(ResourceHandle id)
    {
        WVKComparisonSampler s;
        s.handle = id;
        s.state  = SamplerDescriptorHeap[id];
        return s;
    }

    ResourceHandle Handle() { return handle; }
    SamplerComparisonState   Resource() { return state; }
};

// RO Textures

template<typename T>
class WVKTexture1D
{
    ResourceHandle handle;
    Texture1D<T> texture;

    static WVKTexture1D<T> Create(ResourceHandle id)
    {
        WVKTexture1D<T> t;
        t.handle  = id;
        t.texture = ResourceDescriptorHeap[id];
        return t;
    }

    ResourceHandle Handle() { return handle; }
    Texture1D<T>   Resource() { return texture; }

    T    Load(int2 location)                                                         { return texture.Load(location); }
    T    Sample(WVKSampler s, float location)                                   { return texture.Sample(s.state, location); }
    T    SampleLevel(WVKSampler s, float location, float lod)                   { return texture.SampleLevel(s.state, location, lod); }
    T    SampleBias(WVKSampler s, float location, float bias)                   { return texture.SampleBias(s.state, location, bias); }
    T    SampleGrad(WVKSampler s, float location, float ddx, float ddy)         { return texture.SampleGrad(s.state, location, ddx, ddy); }
    void GetDimensions(uint mip, out uint width, out uint numLevels)                 { texture.GetDimensions(mip, width, numLevels); }
};

template<typename T>
class WVKTexture2D
{
    ResourceHandle handle;
    Texture2D<T> texture;

    static WVKTexture2D<T> Create(ResourceHandle id)
    {
        WVKTexture2D<T> t;
        t.handle  = id;
        t.texture = ResourceDescriptorHeap[id];
        return t;
    }

    ResourceHandle Handle() { return handle; }
    Texture2D<T>   Resource() { return texture; }

    // `location` is a texel coordinate; mip 0 is implied, matching WVKRWTexture2D::Load. Use
    // Resource().Load(int3(xy, mip)) to read a specific mip.
    T    Load(int2 location)                                                         { return texture.Load(int3(location, 0)); }
    T    Sample(WVKSampler s, float2 location)                                   { return texture.Sample(s.state, location); }
    T    SampleLevel(WVKSampler s, float2 location, float lod)                   { return texture.SampleLevel(s.state, location, lod); }
    T    SampleBias(WVKSampler s, float2 location, float bias)                   { return texture.SampleBias(s.state, location, bias); }
    T    SampleGrad(WVKSampler s, float2 location, float2 ddx, float2 ddy)         { return texture.SampleGrad(s.state, location, ddx, ddy); }
    float SampleCmp(WVKComparisonSampler s, float2 location, float cmp)             { return texture.SampleCmp(s.state, location, cmp); }
    float SampleCmpLevelZero(WVKComparisonSampler s, float2 location, float cmp)     { return texture.SampleCmpLevelZero(s.state, location, cmp); }
    // Gather returns one channel from each of the four texels that a bilinear tap would blend,
    // ordered counter-clockwise from the lower-left: (-,+), (+,+), (+,-), (-,-) relative to the
    // sample point. Unlike Sample it needs no LOD argument -- gather is always mip 0 -- so it is
    // usable from a compute shader as-is.
    float4 Gather(WVKSampler s, float2 location)                                    { return texture.Gather(s.state, location); }
    float4 GatherRed(WVKSampler s, float2 location)                                 { return texture.GatherRed(s.state, location); }
    float4 GatherGreen(WVKSampler s, float2 location)                               { return texture.GatherGreen(s.state, location); }
    float4 GatherBlue(WVKSampler s, float2 location)                                { return texture.GatherBlue(s.state, location); }
    float4 GatherAlpha(WVKSampler s, float2 location)                               { return texture.GatherAlpha(s.state, location); }
    void GetDimensions(uint mip, out uint width, out uint height, out uint numLevels)                 { texture.GetDimensions(mip, width, height, numLevels); }
};

template<typename T>
class WVKTexture2DArray
{
    ResourceHandle handle;
    Texture2DArray<T> texture;

    static WVKTexture2DArray<T> Create(ResourceHandle id)
    {
        WVKTexture2DArray<T> t;
        t.handle  = id;
        t.texture = ResourceDescriptorHeap[id];
        return t;
    }

    ResourceHandle Handle() { return handle; }
    Texture2DArray<T>   Resource() { return texture; }

    // `location` is (x, y, slice); mip 0 is implied, matching WVKRWTexture2DArray::Load. Use
    // Resource().Load(int4(xy, slice, mip)) to read a specific mip.
    T    Load(int3 location)                                                         { return texture.Load(int4(location, 0)); }
    T    Sample(WVKSampler s, float3 location)                                   { return texture.Sample(s.state, location); }
    T    SampleLevel(WVKSampler s, float3 location, float lod)                   { return texture.SampleLevel(s.state, location, lod); }
    T    SampleBias(WVKSampler s, float3 location, float bias)                   { return texture.SampleBias(s.state, location, bias); }
    T    SampleGrad(WVKSampler s, float3 location, float3 ddx, float3 ddy)         { return texture.SampleGrad(s.state, location, ddx, ddy); }
    float SampleCmp(WVKComparisonSampler s, float3 location, float cmp)             { return texture.SampleCmp(s.state, location, cmp); }
    float SampleCmpLevelZero(WVKComparisonSampler s, float3 location, float cmp)     { return texture.SampleCmpLevelZero(s.state, location, cmp); }
    void GetDimensions(uint mip, out uint width, out uint height, out uint numLevels, out uint arraySize)                 { texture.GetDimensions(mip, width, height, numLevels, arraySize); }
};

template<typename T>
class WVKTexture3D
{
    ResourceHandle handle;
    Texture3D<T> texture;

    static WVKTexture3D<T> Create(ResourceHandle id)
    {
        WVKTexture3D<T> t;
        t.handle  = id;
        t.texture = ResourceDescriptorHeap[id];
        return t;
    }

    ResourceHandle Handle() { return handle; }
    Texture3D<T>   Resource() { return texture; }

    T    Load(int4 location)                                                         { return texture.Load(location); }
    T    Sample(WVKSampler s, float3 location)                                   { return texture.Sample(s.state, location); }
    T    SampleLevel(WVKSampler s, float3 location, float lod)                   { return texture.SampleLevel(s.state, location, lod); }
    T    SampleBias(WVKSampler s, float3 location, float bias)                   { return texture.SampleBias(s.state, location, bias); }
    T    SampleGrad(WVKSampler s, float3 location, float3 ddx, float3 ddy)         { return texture.SampleGrad(s.state, location, ddx, ddy); }
    void GetDimensions(uint mip, out uint width, out uint height, out uint depth, out uint numLevels)                 { texture.GetDimensions(mip, width, height, depth, numLevels); }
};

template<typename T>
class WVKTextureCube
{
    ResourceHandle handle;
    TextureCube<T> texture;

    static WVKTextureCube<T> Create(ResourceHandle id)
    {
        WVKTextureCube<T> t;
        t.handle  = id;
        t.texture = ResourceDescriptorHeap[id];
        return t;
    }

    ResourceHandle Handle() { return handle; }
    TextureCube<T>   Resource() { return texture; }

    T    Sample(WVKSampler s, float3 location)                                   { return texture.Sample(s.state, location); }
    T    SampleLevel(WVKSampler s, float3 location, float lod)                   { return texture.SampleLevel(s.state, location, lod); }
    T    SampleBias(WVKSampler s, float3 location, float bias)                   { return texture.SampleBias(s.state, location, bias); }
    T    SampleGrad(WVKSampler s, float3 location, float3 ddx, float3 ddy)         { return texture.SampleGrad(s.state, location, ddx, ddy); }
    void GetDimensions(uint mip, out uint width, out uint height, out uint numLevels)                 { texture.GetDimensions(mip, width, height, numLevels); }
};

// RW Textures

template<typename T>
class WVKRWTexture1D
{
    ResourceHandle handle;
    RWTexture1D<T> texture;

    static WVKRWTexture1D<T> Create(ResourceHandle id)
    {
        WVKRWTexture1D<T> t;
        t.handle  = id;
        t.texture = ResourceDescriptorHeap[id];
        return t;
    }

    ResourceHandle Handle() { return handle; }
    RWTexture1D<T>   Resource() { return texture; }

    T Load(int location) { return texture[location]; }
    void Store(int location, T value) { texture[location] = value; }
    void GetDimensions(out uint width) { texture.GetDimensions(width); }
};

template<typename T>
class WVKRWTexture2D
{
    ResourceHandle handle;
    RWTexture2D<T> texture;

    static WVKRWTexture2D<T> Create(ResourceHandle id)
    {
        WVKRWTexture2D<T> t;
        t.handle  = id;
        t.texture = ResourceDescriptorHeap[id];
        return t;
    }

    ResourceHandle Handle() { return handle; }
    RWTexture2D<T>   Resource() { return texture; }

    T Load(int2 location) { return texture[location]; }
    void Store(int2 location, T value) { texture[location] = value; }
    void GetDimensions(out uint width, out uint height) { texture.GetDimensions(width, height); }
};

template<typename T>
class WVKRWTexture2DArray
{
    ResourceHandle handle;
    RWTexture2DArray<T> texture;

    static WVKRWTexture2DArray<T> Create(ResourceHandle id)
    {
        WVKRWTexture2DArray<T> t;
        t.handle  = id;
        t.texture = ResourceDescriptorHeap[id];
        return t;
    }

    ResourceHandle Handle() { return handle; }
    RWTexture2DArray<T>   Resource() { return texture; }

    T Load(int3 location) { return texture[location]; }
    void Store(int3 location, T value) { texture[location] = value; }
    void GetDimensions(out uint width, out uint height, out uint arraySize) { texture.GetDimensions(width, height, arraySize); }
};

template<typename T>
class WVKRWTexture3D
{
    ResourceHandle handle;
    RWTexture3D<T> texture;

    static WVKRWTexture3D<T> Create(ResourceHandle id)
    {
        WVKRWTexture3D<T> t;
        t.handle  = id;
        t.texture = ResourceDescriptorHeap[id];
        return t;
    }

    ResourceHandle Handle() { return handle; }
    RWTexture3D<T>   Resource() { return texture; }

    T Load(uint3 location) { return texture[location]; }
    void Store(uint3 location, T value) { texture[location] = value; }
    void GetDimensions(out uint width, out uint height, out uint depth) { texture.GetDimensions(width, height, depth); }
};

// Buffers

template<typename T>
class WVKStructuredBuffer
{
    ResourceHandle handle;
    StructuredBuffer<T> buffer;

    static WVKStructuredBuffer<T> Create(ResourceHandle id)
    {
        WVKStructuredBuffer<T> b;
        b.handle  = id;
        b.buffer = ResourceDescriptorHeap[id];
        return b;
    }

    ResourceHandle Handle() { return handle; }
    StructuredBuffer<T>   Resource() { return buffer; }

    T Load(uint location) { return buffer[location]; }
    void GetDimensions(out uint count) { buffer.GetDimensions(count); }
};

template<typename T>
class WVKRWStructuredBuffer
{
    ResourceHandle handle;
    
    static WVKRWStructuredBuffer<T> Create(ResourceHandle id)
    {
        WVKRWStructuredBuffer<T> b;
        b.handle = id;
        return b;
    }

    ResourceHandle Handle() { return handle; }
    RWStructuredBuffer<T>   Resource() { RWStructuredBuffer<T> buffer = ResourceDescriptorHeap[handle]; return buffer; }

    T Load(uint location) { RWStructuredBuffer<T> buffer = ResourceDescriptorHeap[handle]; return buffer[location]; }
    void Store(uint location, T value) { RWStructuredBuffer<T> buffer = ResourceDescriptorHeap[handle]; buffer[location] = value; }
    void GetDimensions(out uint count) { RWStructuredBuffer<T> buffer = ResourceDescriptorHeap[handle]; buffer.GetDimensions(count); }
};

class WVKByteAddressBuffer
{
    ResourceHandle handle;
    ByteAddressBuffer buffer;

    static WVKByteAddressBuffer Create(ResourceHandle id)
    {
        WVKByteAddressBuffer b;
        b.handle  = id;
        b.buffer = ResourceDescriptorHeap[id];
        return b;
    }

    ResourceHandle Handle() { return handle; }
    ByteAddressBuffer   Resource() { return buffer; }

    uint Load(uint location) { return buffer.Load(location); }
    uint2 Load2(uint location) { return buffer.Load2(location); }
    uint3 Load3(uint location) { return buffer.Load3(location); }
    uint4 Load4(uint location) { return buffer.Load4(location); }
};

class WVKRWByteAddressBuffer
{
    ResourceHandle handle;
    RWByteAddressBuffer buffer;

    static WVKRWByteAddressBuffer Create(ResourceHandle id)
    {
        WVKRWByteAddressBuffer b;
        b.handle  = id;
        b.buffer = ResourceDescriptorHeap[id];
        return b;
    }

    ResourceHandle Handle() { return handle; }
    RWByteAddressBuffer   Resource() { return buffer; }

    uint Load(uint location) { return buffer.Load(location); }
    uint2 Load2(uint location) { return buffer.Load2(location); }
    uint3 Load3(uint location) { return buffer.Load3(location); }
    uint4 Load4(uint location) { return buffer.Load4(location); }

    void Store(uint location, uint value) { buffer.Store(location, value); }
    void Store2(uint location, uint2 value) { buffer.Store2(location, value); }
    void Store3(uint location, uint3 value) { buffer.Store3(location, value); }
    void Store4(uint location, uint4 value) { buffer.Store4(location, value); }

    void InterlockedAdd(uint addr, uint v, out uint original) { buffer.InterlockedAdd(addr, v, original); }
    void InterlockedAnd(uint addr, uint v, out uint original) { buffer.InterlockedAnd(addr, v, original); }
    void InterlockedOr(uint addr, uint v, out uint original) { buffer.InterlockedOr(addr, v, original); }
    void InterlockedXor(uint addr, uint v, out uint original) { buffer.InterlockedXor(addr, v, original); }
    void InterlockedMax(uint addr, uint v, out uint original) { buffer.InterlockedMax(addr, v, original); }
    void InterlockedMin(uint addr, uint v, out uint original) { buffer.InterlockedMin(addr, v, original); }
    void InterlockedExchange(uint addr, uint v, out uint original) { buffer.InterlockedExchange(addr, v, original); }
    void InterlockedCompareExchange(uint addr, uint compareValue, uint value, out uint original) { buffer.InterlockedCompareExchange(addr, compareValue, value, original); }
};

#define WVK_DECLARE_DRAW_ID() [[vk::ext_builtin_input(4426)]] static const uint __wvk_draw_id;
#define WVK_DRAW_ID() __wvk_draw_id

class WVKIndirectDrawIndexedBundle
{
    WVKRWByteAddressBuffer commands;
    WVKRWByteAddressBuffer count;

    static WVKIndirectDrawIndexedBundle Create(uint64_t bundleHandle)
    {
        WVKIndirectDrawIndexedBundle b;
        b.commands = WVKRWByteAddressBuffer::Create((ResourceHandle)(bundleHandle & 0xFFFFFFFF));
        b.count    = WVKRWByteAddressBuffer::Create((ResourceHandle)(bundleHandle >> 32));
        return b;
    }

    // commandOffset/countIndex are in command-struct/uint32 units, matching wvkIndirectBundleExecuteInfo.
    // Returns the reserved slot: on Vulkan WVK_DRAW_ID() is the linear DrawIndex builtin rather than
    // the patched drawId, so producers that need drawId in the consuming shader must write it into
    // their own indirection buffer at this slot.
    uint DrawIndexed(uint commandOffset, uint countIndex, uint drawId, uint indexCount, uint instanceCount, uint firstIndex, int vertexOffset, uint firstInstance)
    {
        uint slot;
        count.InterlockedAdd(countIndex * 4, 1, slot);
        uint byteOffset = (commandOffset + slot) * 24; // sizeof(wvkDrawIndexedCommand): 6 x 4 bytes
        commands.Store(byteOffset + 0,  drawId);
        commands.Store(byteOffset + 4,  indexCount);
        commands.Store(byteOffset + 8,  instanceCount);
        commands.Store(byteOffset + 12, firstIndex);
        commands.Store(byteOffset + 16, (uint)vertexOffset);
        commands.Store(byteOffset + 20, firstInstance);
        return slot;
    }
};

class WVKIndirectDrawBundle
{
    WVKRWByteAddressBuffer commands;
    WVKRWByteAddressBuffer count;

    static WVKIndirectDrawBundle Create(uint64_t bundleHandle)
    {
        WVKIndirectDrawBundle b;
        b.commands = WVKRWByteAddressBuffer::Create((ResourceHandle)(bundleHandle & 0xFFFFFFFF));
        b.count    = WVKRWByteAddressBuffer::Create((ResourceHandle)(bundleHandle >> 32));
        return b;
    }

    // Returns the reserved slot -- see WVKIndirectDrawIndexedBundle::DrawIndexed.
    uint Draw(uint commandOffset, uint countIndex, uint drawId, uint vertexCount, uint instanceCount, uint firstVertex, uint firstInstance)
    {
        uint slot;
        count.InterlockedAdd(countIndex * 4, 1, slot);
        uint byteOffset = (commandOffset + slot) * 20; // sizeof(wvkDrawCommand): 5 x 4 bytes
        commands.Store(byteOffset + 0,  drawId);
        commands.Store(byteOffset + 4,  vertexCount);
        commands.Store(byteOffset + 8,  instanceCount);
        commands.Store(byteOffset + 12, firstVertex);
        commands.Store(byteOffset + 16, firstInstance);
        return slot;
    }
};

class WVKIndirectDrawMeshBundle
{
    WVKRWByteAddressBuffer commands;
    WVKRWByteAddressBuffer count;

    static WVKIndirectDrawMeshBundle Create(uint64_t bundleHandle)
    {
        WVKIndirectDrawMeshBundle b;
        b.commands = WVKRWByteAddressBuffer::Create((ResourceHandle)(bundleHandle & 0xFFFFFFFF));
        b.count    = WVKRWByteAddressBuffer::Create((ResourceHandle)(bundleHandle >> 32));
        return b;
    }

    // Returns the reserved slot -- see WVKIndirectDrawIndexedBundle::DrawIndexed.
    uint DrawMesh(uint commandOffset, uint countIndex, uint drawId, uint groupSizeX, uint groupSizeY, uint groupSizeZ)
    {
        uint slot;
        count.InterlockedAdd(countIndex * 4, 1, slot);
        uint byteOffset = (commandOffset + slot) * 16; // sizeof(wvkDrawMeshCommand): 4 x 4 bytes
        commands.Store(byteOffset + 0,  drawId);
        commands.Store(byteOffset + 4,  groupSizeX);
        commands.Store(byteOffset + 8,  groupSizeY);
        commands.Store(byteOffset + 12, groupSizeZ);
        return slot;
    }
};

class WVKIndirectDispatchBundle
{
    WVKRWByteAddressBuffer commands;
    WVKRWByteAddressBuffer count;

    static WVKIndirectDispatchBundle Create(uint64_t bundleHandle)
    {
        WVKIndirectDispatchBundle b;
        b.commands = WVKRWByteAddressBuffer::Create((ResourceHandle)(bundleHandle & 0xFFFFFFFF));
        b.count    = WVKRWByteAddressBuffer::Create((ResourceHandle)(bundleHandle >> 32));
        return b;
    }

    void Dispatch(uint commandOffset, uint countIndex, uint groupCountX, uint groupCountY, uint groupCountZ)
    {
        uint slot;
        count.InterlockedAdd(countIndex * 4, 1, slot);
        uint byteOffset = (commandOffset + slot) * 12; // sizeof(wvkDispatchCommand): 3 x 4 bytes
        commands.Store(byteOffset + 0, groupCountX);
        commands.Store(byteOffset + 4, groupCountY);
        commands.Store(byteOffset + 8, groupCountZ);
    }
};

#endif
