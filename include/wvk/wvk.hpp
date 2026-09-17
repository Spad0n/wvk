#pragma once

// wvk -- a thin Vulkan 1.3 abstraction.
//
// Synchronization, error handling and command model follow NoGraphicsAPI: a single queue family,
// resource-free global barriers, timeline semaphores, no exceptions, no RTTI, no STL in the ABI.
// Resource binding follows a bindless descriptor heap: one descriptor set with two bindings, a slot
// allocator, and 32-bit handles pushed through push constants. See shaders/wvk.hlsl for the shader
// side.
//
// The only non-WSI device extension required is VK_EXT_mutable_descriptor_type.
// VK_EXT_mesh_shader, multiDrawIndirect and drawIndirectCount are optional and reported through
// DeviceCaps.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>

namespace wvk
{

// Short aliases over the standard fixed-width types. These exist for brevity in descriptor structs,
// not to avoid the standard library: std::span, std::string and std::byte are used throughout.
using int8 = std::int8_t;
using uint8 = std::uint8_t;
using int16 = std::int16_t;
using uint16 = std::uint16_t;
using int32 = std::int32_t;
using uint32 = std::uint32_t;
using int64 = std::int64_t;
using uint64 = std::uint64_t;
using uintptr = std::uintptr_t;
using byte = std::uint8_t;

// ---------------------------------------------------------------------------------------------
// Push-constant roots
// ---------------------------------------------------------------------------------------------

// The push-constant root of a draw. Any trivially copyable struct works; root_of makes the span.
//
//     const MyRoot root{...};
//     wvk::draw(commands, wvk::root_of(root), 3);
//
// Nothing retains the span past the call, so a temporary is fine.
template<typename T>
    requires std::is_trivially_copyable_v<T>
[[nodiscard]] std::span<const std::byte> root_of(const T& value) noexcept
{
    return std::as_bytes(std::span<const T, 1>{&value, 1});
}

struct uint32x2
{
    uint32 x = 0;
    uint32 y = 0;
};

struct uint32x3
{
    uint32 x = 0;
    uint32 y = 0;
    uint32 z = 0;
};

// ---------------------------------------------------------------------------------------------
// Opaque objects
// ---------------------------------------------------------------------------------------------

struct Device;
struct Heap;
struct TextureHeap;
struct Buffer;
struct Texture;
struct BufferView;
struct TextureView;
struct RenderView;
struct Sampler;
struct PSO;
struct CommandBuffer;
struct TimelineSemaphore;

// ---------------------------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------------------------

enum class Error : uint8
{
    none,
    unsupported,     // No physical device meets the required Vulkan 1.3 feature set.
    device_lost,     // Sticky once set; every later call is a no-op.
    out_of_memory,
    heap_exhausted,  // The bindless descriptor heap ran out of slots.
    driver_error,
};

// ---------------------------------------------------------------------------------------------
// Bindless handles
// ---------------------------------------------------------------------------------------------

// A slot index in the global descriptor set, matching WVK_INVALID_DESCRIPTOR / ResourceHandle in
// shaders/wvk.hlsl. Copy these into a push-constant root struct; never dereference them host-side.
using ResourceHandle = uint32;
inline constexpr ResourceHandle invalid_handle = 0xffffffffu;

// ---------------------------------------------------------------------------------------------
// Formats
// ---------------------------------------------------------------------------------------------

enum class Format : uint8
{
    r8_srgb,
    rg8_srgb,
    rgba8_srgb,
    bgra8_srgb,

    r8_unorm,
    rg8_unorm,
    rgba8_unorm,
    bgra8_unorm,
    r16_unorm,
    rg16_unorm,
    rgba16_unorm,

    r8_uint,
    rg8_uint,
    rgba8_uint,
    r16_uint,
    rg16_uint,
    rgba16_uint,
    r32_uint,
    rg32_uint,
    rgba32_uint,

    r16_float,
    rg16_float,
    rgba16_float,
    r32_float,
    rg32_float,
    rgba32_float,

    rgb10a2_unorm,
    rg11b10_float,

    d16_unorm,
    d32_float,
    d24_unorm_s8_uint,
    d32_float_s8_uint,

    bc1_unorm,
    bc1_srgb,
    bc3_unorm,
    bc3_srgb,
    bc4_unorm,
    bc5_unorm,
    bc6h_ufloat,
    bc7_unorm,
    bc7_srgb,
    astc_4x4_unorm,
    astc_4x4_srgb,

    undefined, // Must remain last; every preceding value is a concrete format.
};

struct FormatInfo
{
    uint32x2 block_extent = {.x = 1, .y = 1};
    uint32 bytes_per_block = 0;
    bool depth = false;
    bool stencil = false;
};

[[nodiscard]] constexpr FormatInfo get_format_info(Format format) noexcept
{
    switch (format)
    {
    case Format::r8_srgb:
    case Format::r8_unorm:
    case Format::r8_uint:
        return {.bytes_per_block = 1};
    case Format::rg8_srgb:
    case Format::rg8_unorm:
    case Format::rg8_uint:
    case Format::r16_unorm:
    case Format::r16_uint:
    case Format::r16_float:
        return {.bytes_per_block = 2};
    case Format::d16_unorm:
        return {.bytes_per_block = 2, .depth = true};
    case Format::rgba8_srgb:
    case Format::bgra8_srgb:
    case Format::rgba8_unorm:
    case Format::bgra8_unorm:
    case Format::rg16_unorm:
    case Format::rgba8_uint:
    case Format::rg16_uint:
    case Format::r32_uint:
    case Format::rg16_float:
    case Format::r32_float:
    case Format::rgb10a2_unorm:
    case Format::rg11b10_float:
        return {.bytes_per_block = 4};
    case Format::d32_float:
        return {.bytes_per_block = 4, .depth = true};
    case Format::d24_unorm_s8_uint:
        return {.bytes_per_block = 4, .depth = true, .stencil = true};
    case Format::rgba16_unorm:
    case Format::rgba16_uint:
    case Format::rg32_uint:
    case Format::rgba16_float:
    case Format::rg32_float:
        return {.bytes_per_block = 8};
    case Format::d32_float_s8_uint:
        return {.bytes_per_block = 8, .depth = true, .stencil = true};
    case Format::rgba32_uint:
    case Format::rgba32_float:
        return {.bytes_per_block = 16};
    case Format::bc1_unorm:
    case Format::bc1_srgb:
    case Format::bc4_unorm:
        return {.block_extent = {.x = 4, .y = 4}, .bytes_per_block = 8};
    case Format::bc3_unorm:
    case Format::bc3_srgb:
    case Format::bc5_unorm:
    case Format::bc6h_ufloat:
    case Format::bc7_unorm:
    case Format::bc7_srgb:
    case Format::astc_4x4_unorm:
    case Format::astc_4x4_srgb:
        return {.block_extent = {.x = 4, .y = 4}, .bytes_per_block = 16};
    case Format::undefined:
        return {};
    }
    return {};
}

// ---------------------------------------------------------------------------------------------
// Memory and resources
// ---------------------------------------------------------------------------------------------

enum class MemoryType : uint8
{
    gpu_only,   // DEVICE_LOCAL. No CPU pointer.
    cpu_to_gpu, // HOST_VISIBLE | HOST_COHERENT. Persistently mapped for upload and per-frame writes.
    gpu_to_cpu, // HOST_VISIBLE | HOST_COHERENT | HOST_CACHED where available. Persistently mapped for readback.
};

// Every buffer is created transfer-capable, indirect-capable, device-address-capable and usable as
// a storage buffer; these flags cost nothing and keep the descriptor model uniform. Only the extra
// capabilities below need opting in.
enum class BufferUsage : uint32
{
    none = 0,
    index = 1u << 0u,          // Bindable through draw_indexed / draw_indexed_indirect.
    uniform_texel = 1u << 1u,  // Viewable as BufferViewType::uniform_texel (HLSL Buffer<T>).
    storage_texel = 1u << 2u,  // Viewable as BufferViewType::storage_texel (HLSL RWBuffer<T>).
};

constexpr BufferUsage operator|(BufferUsage lhs, BufferUsage rhs) noexcept
{
    return static_cast<BufferUsage>(static_cast<uint32>(lhs) | static_cast<uint32>(rhs));
}

enum class TextureType : uint8
{
    one_d,
    two_d,
    two_d_array,
    three_d,
    cube,
};

// Nothing is implied. Vulkan derives framebuffer compression from the usage flags an image is
// created with, so a transfer bit that is requested but never used still costs bandwidth on some
// hardware. Ask for exactly what the texture does.
enum class TextureUsage : uint32
{
    none = 0,
    sampled = 1u << 0u,
    storage = 1u << 1u,
    color_attachment = 1u << 2u,
    depth_stencil_attachment = 1u << 3u,
    transfer_source = 1u << 4u,
    transfer_destination = 1u << 5u,
};

constexpr TextureUsage operator|(TextureUsage lhs, TextureUsage rhs) noexcept
{
    return static_cast<TextureUsage>(static_cast<uint32>(lhs) | static_cast<uint32>(rhs));
}

enum class TextureAspect : uint8
{
    automatic, // Colour, or depth before stencil for combined formats.
    color,
    depth,
    stencil,
};

// The five descriptor types in the mutable list of binding 0. UNIFORM_BUFFER is deliberately absent:
// nothing in shaders/wvk.hlsl consumes it, and leaving it out drops the requirement on
// descriptorBindingUniformBufferUpdateAfterBind, the least widely supported feature of the set.
// Constants travel through push constants and structured buffers instead.
enum class BufferViewType : uint8
{
    structured,    // HLSL StructuredBuffer<T> / RWStructuredBuffer<T>: VK_DESCRIPTOR_TYPE_STORAGE_BUFFER.
    byte_address,  // HLSL ByteAddressBuffer / RWByteAddressBuffer: also a storage buffer.
    uniform_texel, // HLSL Buffer<T>: VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER. Requires BufferUsage::uniform_texel.
    storage_texel, // HLSL RWBuffer<T>: VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER. Requires BufferUsage::storage_texel.
};

enum class TextureViewType : uint8
{
    sampled, // VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE.
    storage, // VK_DESCRIPTOR_TYPE_STORAGE_IMAGE. A cube texture is viewed as a 2D array.
};

struct SizeAlign
{
    uint64 size = 0;
    uint64 align = 0;
};

// A subrange of a buffer. A zero size selects everything from offset to the end of the buffer.
struct BufferRange
{
    Buffer* buffer = nullptr;
    uint64 offset = 0;
    uint64 size = 0;
};

struct BufferDesc
{
    uint64 byte_count = 0;
    BufferUsage usage = BufferUsage::none;
    MemoryType memory = MemoryType::gpu_only;
    Heap* heap = nullptr;    // Null allocates a dedicated VkDeviceMemory for this buffer.
    uint64 heap_offset = 0;  // Must satisfy get_buffer_size_align().
};

struct TextureDesc
{
    TextureType type = TextureType::two_d;
    uint32x3 extent = {.x = 1, .y = 1, .z = 1};
    uint32 mip_levels = 1;
    uint32 layer_count = 1; // Vulkan array layers; a cube's six faces are individual layers.
    Format format = Format::rgba8_unorm;
    TextureUsage usage = TextureUsage::sampled;
    // Lets a texture view reinterpret the format (unorm read as srgb, say). Off by default: the
    // VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT it implies can cost framebuffer compression.
    bool mutable_format = false;
    TextureHeap* heap = nullptr; // Null allocates a dedicated VkDeviceMemory for this texture.
    uint64 heap_offset = 0;      // Must satisfy get_texture_size_align().
};

struct BufferViewDesc
{
    Buffer* buffer = nullptr;
    BufferViewType type = BufferViewType::structured;
    uint64 offset = 0;
    uint64 size = 0;                    // Zero runs to the end of the buffer.
    Format texel_format = Format::undefined; // Required for the texel view types, ignored otherwise.
};

struct TextureViewDesc
{
    Texture* texture = nullptr;
    TextureViewType type = TextureViewType::sampled;
    Format format = Format::undefined;               // Undefined inherits the texture format.
    TextureAspect aspect = TextureAspect::automatic;
    uint32 base_mip = 0;
    uint32 mip_count = 0;   // Zero selects every remaining mip level.
    uint32 base_layer = 0;
    uint32 layer_count = 0; // Zero selects every remaining layer.
};

// A single mip and single layer, used as a colour, depth or stencil attachment.
struct RenderViewDesc
{
    uint32 mip_level = 0;
    uint32 layer = 0;
};

enum class Filter : uint8
{
    nearest,
    linear,
};

enum class AddressMode : uint8
{
    repeat,
    mirrored_repeat,
    clamp_to_edge,
    clamp_to_border,
};

enum class CompareOp : uint8
{
    never,
    less,
    equal,
    less_equal,
    greater,
    not_equal,
    greater_equal,
    always,
};

struct SamplerDesc
{
    Filter min_filter = Filter::linear;
    Filter mag_filter = Filter::linear;
    Filter mip_filter = Filter::linear;
    AddressMode address_u = AddressMode::repeat;
    AddressMode address_v = AddressMode::repeat;
    AddressMode address_w = AddressMode::repeat;
    float max_anisotropy = 1.0f; // Clamped to DeviceCaps::max_anisotropy. 1.0 disables anisotropy.
    float min_lod = 0.0f;
    float max_lod = 1000.0f;
    bool compare_enabled = false; // Produces a SamplerComparisonState / WVKComparisonSampler.
    CompareOp compare = CompareOp::less_equal;
};

// Describes one buffer <-> texture copy region. Zero pitches mean tightly packed.
struct TextureCopyDesc
{
    uint32 mip_level = 0;
    uint32 base_layer = 0;
    uint32 layer_count = 1;
    uint32x3 offset = {};
    uint32x3 extent = {}; // Zero components select the remaining mip extent.
    uint64 row_pitch_bytes = 0;
    uint64 slice_pitch_bytes = 0;
};

// Describes one texture <-> texture copy region. Source and destination must be format-compatible
// in the Vulkan sense: same texel block size, or same block footprint for compressed formats.
struct TextureToTextureCopyDesc
{
    uint32 source_mip = 0;
    uint32 source_base_layer = 0;
    uint32 destination_mip = 0;
    uint32 destination_base_layer = 0;
    uint32 layer_count = 1;
    uint32x3 source_offset = {};
    uint32x3 destination_offset = {};
    uint32x3 extent = {}; // Zero components select the remaining source mip extent.
};

// ---------------------------------------------------------------------------------------------
// Synchronization
// ---------------------------------------------------------------------------------------------

// Ordered by Vulkan's logical execution order where stages are comparable. A barrier's before scope
// covers the selected stages and every logically earlier one; its after scope covers the selected
// stages and every logically later one. vertex and task/mesh are alternative graphics branches,
// depth_stencil_tests spans early and late tests around fragment, compute and transfer are separate
// pipelines, host is a pseudo-stage, and none / all_commands are special masks.
//
// Stage::task and Stage::mesh resolve to nothing on a device that lacks the matching capability;
// see DeviceCaps::task_shaders and DeviceCaps::mesh_shaders.
enum class Stage : uint64
{
    none = 0,
    indirect = 1ull << 0u,
    index_input = 1ull << 1u,
    vertex = 1ull << 2u,
    task = 1ull << 3u,
    mesh = 1ull << 4u,
    depth_stencil_tests = 1ull << 5u,
    fragment = 1ull << 6u,
    color_output = 1ull << 7u,
    compute = 1ull << 8u,
    transfer = 1ull << 9u,
    host = 1ull << 10u,         // Barrier destination only, paired with Access::host_read.
    all_commands = 1ull << 11u, // Every GPU command stage; excludes host.
};

constexpr Stage operator|(Stage lhs, Stage rhs) noexcept
{
    return static_cast<Stage>(static_cast<uint64>(lhs) | static_cast<uint64>(rhs));
}

enum class Access : uint64
{
    none = 0,
    transfer_read = 1ull << 0u,
    transfer_write = 1ull << 1u,
    shader_read = 1ull << 2u,  // Covers both sampled and storage reads.
    shader_write = 1ull << 3u,
    color_read = 1ull << 4u,
    color_write = 1ull << 5u,
    depth_stencil_read = 1ull << 6u,
    depth_stencil_write = 1ull << 7u,
    indirect_read = 1ull << 8u,
    index_read = 1ull << 9u,
    host_read = 1ull << 10u,
};

constexpr Access operator|(Access lhs, Access rhs) noexcept
{
    return static_cast<Access>(static_cast<uint64>(lhs) | static_cast<uint64>(rhs));
}

struct TimelinePoint
{
    TimelineSemaphore* semaphore = nullptr;
    uint64 value = 0;
};

// ---------------------------------------------------------------------------------------------
// Pipeline state
// ---------------------------------------------------------------------------------------------

enum class CullMode : uint8
{
    none,
    clockwise,
    counter_clockwise,
};

enum class FillMode : uint8
{
    solid,
    wireframe, // Requires DeviceCaps::fill_mode_non_solid.
};

enum class Topology : uint8
{
    points,
    lines,
    line_strip,
    triangles,
    triangle_strip,
};

enum class BlendFactor : uint8
{
    zero,
    one,
    source_color,
    one_minus_source_color,
    destination_color,
    one_minus_destination_color,
    source_alpha,
    one_minus_source_alpha,
    destination_alpha,
    one_minus_destination_alpha,
    source_alpha_saturate,
};

enum class BlendOp : uint8
{
    add,
    subtract,
    reverse_subtract,
    minimum,
    maximum,
};

enum class StencilOp : uint8
{
    keep,
    zero,
    replace,
    increment_clamp,
    decrement_clamp,
    invert,
    increment_wrap,
    decrement_wrap,
};

enum class IndexType : uint8
{
    uint16,
    uint32,
};

enum class LoadOp : uint8
{
    load,
    clear,
    discard,
};

enum class StoreOp : uint8
{
    store,
    discard,
};

struct BlendComponentState
{
    BlendFactor source = BlendFactor::one;
    BlendFactor destination = BlendFactor::zero;
    BlendOp operation = BlendOp::add;
};

struct BlendState
{
    bool enabled = false;
    BlendComponentState color = {};
    BlendComponentState alpha = {};
};

struct ColorTargetDesc
{
    Format format = Format::undefined;
    BlendState blend = {};
    uint8 write_mask = 0xf;
};

struct RasterizationState
{
    CullMode cull = CullMode::none;
    FillMode fill = FillMode::solid;
    float depth_bias_constant = 0.0f;
    float depth_bias_clamp = 0.0f;
    float depth_bias_slope = 0.0f;
    bool depth_clamp = false; // Requires DeviceCaps::depth_clamp.
};

struct StencilFaceState
{
    CompareOp compare = CompareOp::always;
    StencilOp fail = StencilOp::keep;
    StencilOp pass = StencilOp::keep;
    StencilOp depth_fail = StencilOp::keep;
    uint8 reference = 0;
};

struct DepthStencilState
{
    bool depth_test = false;
    bool depth_write = false;
    CompareOp depth_compare = CompareOp::less_equal;
    bool stencil_test = false;
    uint8 stencil_read_mask = 0xff;
    uint8 stencil_write_mask = 0xff;
    StencilFaceState front = {};
    StencilFaceState back = {};
};

// There is no vertex input layout anywhere in wvk. Vertex shaders pull their own data from a
// structured buffer indexed by SV_VertexID; see WVKStructuredBuffer in shaders/wvk.hlsl.
struct GraphicsPSODesc
{
    std::span<const uint32> vertex_spirv = {};
    std::span<const uint32> fragment_spirv = {}; // Empty omits the fragment stage, for depth-only passes.
    std::span<const ColorTargetDesc> color_targets = {};
    Format depth_format = Format::undefined;
    Format stencil_format = Format::undefined;
    Topology topology = Topology::triangles;
    RasterizationState rasterization = {};
};

// Requires DeviceCaps::mesh_shaders.
struct MeshPSODesc
{
    // Optional amplification stage, run before the mesh stage. Empty launches mesh workgroups
    // directly; otherwise the draw's group counts launch task workgroups, which cull or expand work
    // and dispatch mesh workgroups through their payload. Requires DeviceCaps::task_shaders.
    std::span<const uint32> task_spirv = {};
    std::span<const uint32> mesh_spirv = {};
    std::span<const uint32> fragment_spirv = {};
    std::span<const ColorTargetDesc> color_targets = {};
    Format depth_format = Format::undefined;
    Format stencil_format = Format::undefined;
    RasterizationState rasterization = {};
};

struct ClearColor
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct ColorAttachment
{
    RenderView* render_view = nullptr;
    LoadOp load = LoadOp::load;
    StoreOp store = StoreOp::store;
    ClearColor clear = {};
};

struct DepthAttachment
{
    RenderView* render_view = nullptr;
    LoadOp load = LoadOp::load;
    StoreOp store = StoreOp::store;
    float clear = 1.0f;
};

struct StencilAttachment
{
    RenderView* render_view = nullptr;
    LoadOp load = LoadOp::load;
    StoreOp store = StoreOp::store;
    uint8 clear = 0;
};

// At most eight colour attachments. At least one attachment is needed to infer the render area.
struct RenderingDesc
{
    std::span<const ColorAttachment> colors = {};
    DepthAttachment depth = {};
    StencilAttachment stencil = {};
};

// ---------------------------------------------------------------------------------------------
// Indirect command ABI
// ---------------------------------------------------------------------------------------------

// These match the byte layouts written by the WVKIndirect*Bundle classes in shaders/wvk.hlsl. Each
// draw command carries a leading draw_id that Vulkan itself does not consume: pass a BufferRange
// whose offset already skips it (see indirect_draw_id_bytes) and use the matching stride.
//
// On Vulkan, WVK_DRAW_ID() is the SPIR-V DrawIndex builtin, which counts linearly from zero rather
// than reproducing draw_id. A producer that needs its own identifier in the consuming shader must
// read it back out of this field through an indirection buffer.

inline constexpr uint32 indirect_draw_id_bytes = 4;

struct DrawCommand
{
    uint32 draw_id;
    uint32 vertex_count;
    uint32 instance_count;
    uint32 first_vertex;
    uint32 first_instance;
};
static_assert(sizeof(DrawCommand) == 20);

struct DrawIndexedCommand
{
    uint32 draw_id;
    uint32 index_count;
    uint32 instance_count;
    uint32 first_index;
    int32 vertex_offset;
    uint32 first_instance;
};
static_assert(sizeof(DrawIndexedCommand) == 24);

struct DrawMeshCommand
{
    uint32 draw_id;
    uint32 group_count_x;
    uint32 group_count_y;
    uint32 group_count_z;
};
static_assert(sizeof(DrawMeshCommand) == 16);

// Dispatch has no draw_id prefix; it is consumed by Vulkan as-is.
struct DispatchCommand
{
    uint32 group_count_x;
    uint32 group_count_y;
    uint32 group_count_z;
};
static_assert(sizeof(DispatchCommand) == 12);

// ---------------------------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------------------------

struct DeviceCaps
{
    std::string device_name;
    uint32 max_push_constant_bytes = 0; // Upper bound on the root span passed to every draw.
    uint32 max_resource_descriptors = 0; // Slots in binding 0, after clamping to driver limits.
    uint32 max_sampler_descriptors = 0;  // Slots in binding 1.
    uint32 max_color_attachments = 0;
    uint32 max_draw_indirect_count = 0;
    uint64 buffer_offset_alignment = 0;  // minStorageBufferOffsetAlignment.
    uint64 texel_buffer_offset_alignment = 0;
    uint64 heap_alignment = 0;           // A common element size for buffer heaps.
    uint64 texture_heap_alignment = 0;   // A common element size for texture heaps; see create_texture_heap.
    float max_anisotropy = 1.0f;
    float timestamp_period_ns = 0.0f;    // Nanoseconds per timestamp tick.

    // Optional capabilities. Nothing below blocks device creation.
    bool mesh_shaders = false;         // VK_EXT_mesh_shader.
    // The task (amplification) stage of VK_EXT_mesh_shader. A separate feature bit from meshShader,
    // so a device can offer mesh without it; MeshPSODesc::task_spirv needs this one.
    bool task_shaders = false;
    bool multi_draw_indirect = false;  // A draw_count above one issues a single command when true.
    bool draw_indirect_count = false;  // GPU-side count buffers; see the *_indirect_count entry points.
    bool depth_clamp = false;
    bool fill_mode_non_solid = false;
    bool texture_compression_bc = false;
    bool texture_compression_astc = false;
    bool timestamps = false;         // The queue reports usable timestampValidBits.
    bool swapchain_storage = false;  // The surface allows STORAGE usage on its images.

    // The format the swapchain was actually created with. DeviceDesc::swapchain_format is only a
    // request: if the surface does not expose it, wvk falls back to the surface's first nameable
    // format. Build colour targets from this value, never from the requested one, or the PSO will
    // not match the render pass. Format::undefined on a headless device.
    Format swapchain_format = Format::undefined;
};

// Which windowing protocol to build a surface for. Consulted on Linux only; Win32 ignores it.
enum class DisplayServer : uint8
{
    x11,     // Xlib. Enables VK_KHR_xlib_surface.
    xcb,     // Enables VK_KHR_xcb_surface.
    wayland, // Enables VK_KHR_wayland_surface.
};

// The native handle pair the Linux surface path consumes. Point DeviceDesc::handle at one of these;
// how the fields are read depends on DeviceDesc::display_server. wvk never includes an X11 or
// Wayland header, so nothing here is typed beyond a pointer and an integer, and building on Linux
// needs no windowing dev packages.
struct LinuxWindowHandle
{
    void* display = nullptr; // Display* (X11), xcb_connection_t* (XCB), or wl_display* (Wayland).
    uint64 window = 0;       // Window / xcb_window_t zero-extended, or a wl_surface* cast to uint64.
};

struct DeviceDesc
{
    // On Win32, the HWND itself. On Linux, a pointer to a LinuxWindowHandle that must outlive this
    // call, though not the device. Null creates a headless device with no swapchain.
    void* handle = nullptr;
    DisplayServer display_server = DisplayServer::x11;
    Format swapchain_format = Format::bgra8_unorm;
    // The drawable size in pixels. Required on Wayland, where the surface never reports a size and
    // this is the only source of truth; ignored on Win32 and X11, which report their own. Always
    // supply it and keep it current through set_drawable_extent, and the same code works on all
    // three.
    uint32x2 drawable_extent = {};
    uint32 desired_swapchain_image_count = 3;
    uint32 frames_in_flight = 2;
    uint32 max_resource_descriptors = 500'000; // Clamped to the driver's update-after-bind limits.
    uint32 max_sampler_descriptors = 1'024;
    uint32 timestamp_query_count = 0;   // Timestamp markers available per command buffer. Zero disables them.
    // Adds STORAGE usage to swapchain images so compute can write them directly. Off by default: it
    // can disable display compression on the presented surface.
    bool request_swapchain_storage = false;
    bool enable_validation = false;
};

struct DeviceInit
{
    Device* device = nullptr;
    Error error = Error::none;
};

struct SwapchainFrame
{
    RenderView* render_view = nullptr; // Null while the drawable extent is zero.
    uint32x2 extent = {};
};

// ---------------------------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------------------------
//
// A windowed device, and every call made with it, must stay on the window's message-pump thread.
// The window must outlive the device.
//
// All destruction is immediate. Destroy a resource only once no recorded or executing submission
// still references it, and drain every submission before destroying the device.

[[nodiscard]] DeviceInit create_device(const DeviceDesc& desc = {}) noexcept;
void destroy_device(Device* device) noexcept;
[[nodiscard]] const DeviceCaps& get_device_caps(const Device* device) noexcept;

// Sticky: once an unrecoverable failure occurs, this keeps reporting it and every subsequent call
// becomes a no-op returning null or zero.
[[nodiscard]] Error get_device_error(const Device* device) noexcept;

[[nodiscard]] bool supports_format(const Device* device, Format format, TextureUsage usage) noexcept;
void wait_idle(Device* device) noexcept;

// Timeline semaphores are the only synchronization primitive the application ever sees.
[[nodiscard]] TimelineSemaphore* create_timeline_semaphore(Device* device, uint64 initial_value = 0) noexcept;
void destroy_timeline_semaphore(TimelineSemaphore* semaphore) noexcept;
[[nodiscard]] uint64 completed_value(const TimelineSemaphore* semaphore) noexcept;
void wait_timeline(TimelinePoint point) noexcept;

// Presentation. acquire() returns an empty frame while the window is minimized; skip the frame.
[[nodiscard]] uint32x2 get_drawable_extent(Device* device) noexcept;

// Tells wvk the drawable size in pixels. Call it whenever the window resizes, or unconditionally
// every frame. On a surface that reports its own extent this is only a fallback; on Wayland it is
// the sole resize signal, because the compositor never tells a client how large it is and
// VK_ERROR_OUT_OF_DATE_KHR alone will not arrive. A change from the current swapchain size marks
// the swapchain for rebuild on the next acquire.
void set_drawable_extent(Device* device, uint32x2 extent) noexcept;
[[nodiscard]] SwapchainFrame acquire(Device* device) noexcept;

// Heaps are one VkDeviceMemory the application suballocates itself. A heap must outlive every
// resource placed in it; placements must not overlap, and must not be recycled before the timeline
// point covering their last use has completed. wvk_utility.hpp provides allocators for both kinds.
//
// Buffers and textures use separate heap types on purpose. A buffer heap picks its memory type from
// the requirements of the buffers placed in it, whereas an image's memoryTypeBits can be strictly
// narrower, so one heap serving both would bind fine for buffers and fail for some images. Keeping
// linear and optimally-tiled resources in separate allocations also removes bufferImageGranularity
// padding from the picture entirely.
[[nodiscard]] Heap* create_heap(Device* device, uint64 byte_count, MemoryType memory = MemoryType::gpu_only) noexcept;
void destroy_heap(Heap* heap) noexcept;
[[nodiscard]] byte* mapped_pointer(Heap* heap) noexcept; // Null for a gpu_only heap.
[[nodiscard]] SizeAlign get_buffer_size_align(Device* device, const BufferDesc& desc) noexcept;

// Texture heaps use one device-wide GPU-only memory type, chosen at device creation by intersecting
// the memoryTypeBits of a representative set of images, so any texture wvk can create will bind.
// DeviceCaps::texture_heap_alignment is a common allocator element size: every alignment returned by
// get_texture_size_align divides it, which avoids per-placement leading padding.
[[nodiscard]] TextureHeap* create_texture_heap(Device* device, uint64 byte_count) noexcept;
void destroy_texture_heap(TextureHeap* heap) noexcept;
[[nodiscard]] SizeAlign get_texture_size_align(Device* device, const TextureDesc& desc) noexcept;

// Buffers. A cpu_to_gpu or gpu_to_cpu buffer is mapped for its whole lifetime, so there is no
// map/unmap pair to get wrong; the memory is coherent, so no flush or invalidate is needed either.
[[nodiscard]] Buffer* create_buffer(Device* device, const BufferDesc& desc) noexcept;
void destroy_buffer(Buffer* buffer) noexcept;
[[nodiscard]] byte* mapped_pointer(Buffer* buffer) noexcept; // Null for a gpu_only buffer.
[[nodiscard]] uint64 get_buffer_size(const Buffer* buffer) noexcept;

// Textures live permanently in VK_IMAGE_LAYOUT_GENERAL. That is what lets barrier() name no
// resource at all; the cost is losing framebuffer compression on hardware that needs a dedicated
// layout for it. Create every texture before beginning commands: the first command buffer begun
// emits their one-time initialization and must therefore be submitted first.
[[nodiscard]] Texture* create_texture(Device* device, const TextureDesc& desc) noexcept;
void destroy_texture(Texture* texture) noexcept;
[[nodiscard]] TextureDesc get_texture_desc(const Texture* texture) noexcept;

// Views allocate a slot in the global descriptor set and write one descriptor into it. The returned
// handle is the value shaders index the heap with. Destroying a view frees the slot for reuse.
[[nodiscard]] BufferView* create_buffer_view(Device* device, const BufferViewDesc& desc) noexcept;
void destroy_buffer_view(BufferView* view) noexcept;
[[nodiscard]] ResourceHandle get_handle(const BufferView* view) noexcept;

[[nodiscard]] TextureView* create_texture_view(Device* device, const TextureViewDesc& desc) noexcept;
void destroy_texture_view(TextureView* view) noexcept;
[[nodiscard]] ResourceHandle get_handle(const TextureView* view) noexcept;

[[nodiscard]] Sampler* create_sampler(Device* device, const SamplerDesc& desc = {}) noexcept;
void destroy_sampler(Sampler* sampler) noexcept;
[[nodiscard]] ResourceHandle get_handle(const Sampler* sampler) noexcept;

// A render view is an attachment, not a bindless resource; it consumes no descriptor slot.
[[nodiscard]] RenderView* create_render_view(Texture* texture, const RenderViewDesc& desc = {}) noexcept;
void destroy_render_view(RenderView* render_view) noexcept;

[[nodiscard]] PSO* create_graphics_pso(Device* device, const GraphicsPSODesc& desc) noexcept;
[[nodiscard]] PSO* create_mesh_pso(Device* device, const MeshPSODesc& desc) noexcept;
[[nodiscard]] PSO* create_compute_pso(Device* device, std::span<const uint32> compute_spirv) noexcept;
void destroy_pso(PSO* pso) noexcept;

// Every command buffer begun must appear exactly once in the next submit or submit_and_present.
[[nodiscard]] CommandBuffer* begin_commands(Device* device) noexcept;
void submit(Device* device, std::span<CommandBuffer* const> commands, TimelinePoint completion) noexcept;
void submit_and_present(Device* device, std::span<CommandBuffer* const> commands, TimelinePoint completion) noexcept;

// Writes one GPU timestamp, in ticks, to an 8-byte-aligned location in a gpu_to_cpu buffer. At most
// DeviceDesc::timestamp_query_count markers per command buffer, and `stage` must name a single
// pipeline stage. Results are resolved when the command buffer ends, so the destination is readable
// through mapped_pointer once the submission's timeline point completes. Multiply by
// DeviceCaps::timestamp_period_ns for nanoseconds. A no-op when DeviceCaps::timestamps is false.
void write_timestamp(CommandBuffer* commands, BufferRange destination, Stage stage = Stage::all_commands) noexcept;

// The whole synchronization model. No resource is named: every texture is already in GENERAL and
// every buffer is plain memory, so a global memory dependency is all Vulkan needs.
void barrier(CommandBuffer* commands, Stage before, Access before_access, Stage after, Access after_access) noexcept;

void copy_buffer(CommandBuffer* commands, BufferRange source, BufferRange destination) noexcept;
void copy_buffer_to_texture(CommandBuffer* commands, BufferRange source, Texture* destination,
                            const TextureCopyDesc& copy = {}) noexcept;
void copy_texture_to_buffer(CommandBuffer* commands, Texture* source, BufferRange destination,
                            const TextureCopyDesc& copy = {}) noexcept;
void copy_texture_to_texture(CommandBuffer* commands, Texture* source, Texture* destination,
                             const TextureToTextureCopyDesc& copy = {}) noexcept;

// begin_render_pass resets viewport and scissor to the full render area and disables depth and
// stencil; the setters below override those defaults until the pass ends.
void begin_render_pass(CommandBuffer* commands, const RenderingDesc& desc) noexcept;
void end_render_pass(CommandBuffer* commands) noexcept;
void set_viewport(CommandBuffer* commands, float x, float y, float width, float height,
                  float min_depth = 0.0f, float max_depth = 1.0f) noexcept;
void set_scissor(CommandBuffer* commands, int32 x, int32 y, uint32 width, uint32 height) noexcept;
void set_depth_stencil(CommandBuffer* commands, const DepthStencilState& state) noexcept;
void bind_pso(CommandBuffer* commands, const PSO* pso) noexcept;

// `root` is the push-constant block: a trivially copyable struct of handles, offsets and constants,
// at most DeviceCaps::max_push_constant_bytes long.
void draw(CommandBuffer* commands, std::span<const std::byte> root, uint32 vertex_count, uint32 instance_count = 1,
          uint32 first_vertex = 0, uint32 first_instance = 0) noexcept;
void draw_indexed(CommandBuffer* commands, std::span<const std::byte> root, BufferRange indices, IndexType type,
                  uint32 index_count, uint32 instance_count = 1, uint32 first_index = 0,
                  int32 vertex_offset = 0, uint32 first_instance = 0) noexcept;
void dispatch(CommandBuffer* commands, std::span<const std::byte> root, uint32x3 group_count) noexcept;

// `group_count` launches task workgroups when the bound PSO has a task stage, and mesh workgroups
// otherwise. The two have separate driver limits, so a count valid for one is not automatically
// valid for the other.
void draw_meshlets(CommandBuffer* commands, std::span<const std::byte> root, uint32x3 group_count) noexcept;

// Indirect draws. When draw_count exceeds one and DeviceCaps::multi_draw_indirect is false, these
// unroll into draw_count single-draw commands rather than failing, so the calling code needs no
// branch; the cap is worth checking only to size the workload.
void draw_indirect(CommandBuffer* commands, std::span<const std::byte> root, BufferRange arguments,
                   uint32 draw_count = 1, uint32 stride = static_cast<uint32>(sizeof(DrawCommand))) noexcept;
void draw_indexed_indirect(CommandBuffer* commands, std::span<const std::byte> root, BufferRange indices, IndexType type,
                           BufferRange arguments, uint32 draw_count = 1,
                           uint32 stride = static_cast<uint32>(sizeof(DrawIndexedCommand))) noexcept;
void dispatch_indirect(CommandBuffer* commands, std::span<const std::byte> root, BufferRange arguments) noexcept;
void draw_meshlets_indirect(CommandBuffer* commands, std::span<const std::byte> root, BufferRange arguments,
                            uint32 draw_count = 1, uint32 stride = static_cast<uint32>(sizeof(DrawMeshCommand))) noexcept;

// GPU-driven counts. These require DeviceCaps::draw_indirect_count and are no-ops without it, since
// there is no host-side way to read the count back in the same submission. `count` addresses one
// uint32. `max_draw_count` bounds the reservation and is clamped to DeviceCaps::max_draw_indirect_count.
void draw_indirect_count(CommandBuffer* commands, std::span<const std::byte> root, BufferRange arguments, BufferRange count,
                         uint32 max_draw_count, uint32 stride = static_cast<uint32>(sizeof(DrawCommand))) noexcept;
void draw_indexed_indirect_count(CommandBuffer* commands, std::span<const std::byte> root, BufferRange indices, IndexType type,
                                 BufferRange arguments, BufferRange count, uint32 max_draw_count,
                                 uint32 stride = static_cast<uint32>(sizeof(DrawIndexedCommand))) noexcept;
void draw_meshlets_indirect_count(CommandBuffer* commands, std::span<const std::byte> root, BufferRange arguments,
                                  BufferRange count, uint32 max_draw_count,
                                  uint32 stride = static_cast<uint32>(sizeof(DrawMeshCommand))) noexcept;

} // namespace wvk
