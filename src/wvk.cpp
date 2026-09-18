#include <wvk/wvk.hpp>

#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#define WVK_LINUX_SURFACE 1
#endif

#include <vulkan/vulkan.h>

#include <cstring>
#include <new>
#include <vector>
#include <iostream>

namespace wvk
{
namespace
{

// ---------------------------------------------------------------------------------------------
// 1. Platform and small utilities
// ---------------------------------------------------------------------------------------------

#if defined(WVK_LINUX_SURFACE)
// The Linux surface path is written against the wire layout rather than the platform headers, the
// way agfx does it, so building wvk needs no X11 or Wayland development packages and one binary can
// serve X11, XCB and Wayland. The three create-info structs below mirror their Vulkan counterparts
// exactly; the entry points are resolved by name at runtime.
//
// The sType values are spelled numerically because vulkan.h only declares the enumerators when the
// matching VK_USE_PLATFORM macro is defined, which is precisely what is being avoided here.
constexpr VkStructureType structure_type_xlib_surface    = static_cast<VkStructureType>(1000004000);
constexpr VkStructureType structure_type_xcb_surface     = static_cast<VkStructureType>(1000005000);
constexpr VkStructureType structure_type_wayland_surface = static_cast<VkStructureType>(1000006000);

struct XlibSurfaceCreateInfo
{
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    void* dpy;            // Display*
    unsigned long window; // Window
};

struct XcbSurfaceCreateInfo
{
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    void* connection; // xcb_connection_t*
    uint32 window;    // xcb_window_t
};

struct WaylandSurfaceCreateInfo
{
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    void* display; // wl_display*
    void* surface; // wl_surface*
};

using PFN_CreatePlatformSurface = VkResult(VKAPI_PTR*)(VkInstance, const void*, const VkAllocationCallbacks*,
                                                       VkSurfaceKHR*);

[[nodiscard]] constexpr const char* surface_extension_of(DisplayServer server) noexcept
{
    switch (server)
    {
    case DisplayServer::x11:     return "VK_KHR_xlib_surface";
    case DisplayServer::xcb:     return "VK_KHR_xcb_surface";
    case DisplayServer::wayland: return "VK_KHR_wayland_surface";
    }
    return "VK_KHR_xlib_surface";
}

[[nodiscard]] constexpr const char* surface_entry_point_of(DisplayServer server) noexcept
{
    switch (server)
    {
    case DisplayServer::x11:     return "vkCreateXlibSurfaceKHR";
    case DisplayServer::xcb:     return "vkCreateXcbSurfaceKHR";
    case DisplayServer::wayland: return "vkCreateWaylandSurfaceKHR";
    }
    return "vkCreateXlibSurfaceKHR";
}
#endif

constexpr uint32 max_color_attachments     = 8;
constexpr uint32 max_enumerated_extensions = 512;
constexpr uint32 max_physical_devices      = 16;
constexpr uint32 max_queue_families        = 32;
constexpr uint32 max_swapchain_images      = 16;
constexpr uint32 max_push_constant_bytes   = 128;

template<typename T>
[[nodiscard]] constexpr T minimum(T lhs, T rhs) noexcept
{
    return lhs < rhs ? lhs : rhs;
}

template<typename T>
[[nodiscard]] constexpr T maximum(T lhs, T rhs) noexcept
{
    return lhs > rhs ? lhs : rhs;
}

template<typename E>
[[nodiscard]] constexpr bool has_flag(E value, E flag) noexcept
{
    using U = __underlying_type(E);
    return (static_cast<U>(value) & static_cast<U>(flag)) != 0;
}

// TODO: need to replace create_object and destroy_object to be C API friendly
template<typename T, typename... Args>
[[nodiscard]] T* create_object(Args&&... args) noexcept
{
    return new (std::nothrow) T{static_cast<Args&&>(args)...};
}

template<typename T>
void destroy_object(T* object) noexcept
{
    delete object;
}

[[nodiscard]] bool has_extension(const VkExtensionProperties* extensions, uint32 count, const char* name) noexcept
{
    for (uint32 index = 0; index < count; ++index)
    {
        if (strcmp(extensions[index].extensionName, name) == 0) return true;
    }
    return false;
}

[[nodiscard]] constexpr uint64 align_up(uint64 value, uint64 alignment) noexcept
{
    return alignment == 0 ? value : (value + alignment - 1) / alignment * alignment;
}

// ---------------------------------------------------------------------------------------------
// 2. Enum translation
// ---------------------------------------------------------------------------------------------

[[nodiscard]] Error error_from_vk(VkResult result) noexcept
{
    switch (result)
    {
    case VK_SUCCESS:
    case VK_SUBOPTIMAL_KHR:
        return Error::none;
    case VK_ERROR_DEVICE_LOST:
    case VK_ERROR_SURFACE_LOST_KHR:
        return Error::device_lost;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    case VK_ERROR_OUT_OF_POOL_MEMORY:
        return Error::out_of_memory;
    case VK_ERROR_EXTENSION_NOT_PRESENT:
    case VK_ERROR_FEATURE_NOT_PRESENT:
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return Error::unsupported;
    default:
        return Error::driver_error;
    }
}

[[nodiscard]] constexpr VkFormat to_vk(Format format) noexcept
{
    switch (format)
    {
    case Format::r8_srgb:           return VK_FORMAT_R8_SRGB;
    case Format::rg8_srgb:          return VK_FORMAT_R8G8_SRGB;
    case Format::rgba8_srgb:        return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::bgra8_srgb:        return VK_FORMAT_B8G8R8A8_SRGB;
    case Format::r8_unorm:          return VK_FORMAT_R8_UNORM;
    case Format::rg8_unorm:         return VK_FORMAT_R8G8_UNORM;
    case Format::rgba8_unorm:       return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::bgra8_unorm:       return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::r16_unorm:         return VK_FORMAT_R16_UNORM;
    case Format::rg16_unorm:        return VK_FORMAT_R16G16_UNORM;
    case Format::rgba16_unorm:      return VK_FORMAT_R16G16B16A16_UNORM;
    case Format::r8_uint:           return VK_FORMAT_R8_UINT;
    case Format::rg8_uint:          return VK_FORMAT_R8G8_UINT;
    case Format::rgba8_uint:        return VK_FORMAT_R8G8B8A8_UINT;
    case Format::r16_uint:          return VK_FORMAT_R16_UINT;
    case Format::rg16_uint:         return VK_FORMAT_R16G16_UINT;
    case Format::rgba16_uint:       return VK_FORMAT_R16G16B16A16_UINT;
    case Format::r32_uint:          return VK_FORMAT_R32_UINT;
    case Format::rg32_uint:         return VK_FORMAT_R32G32_UINT;
    case Format::rgba32_uint:       return VK_FORMAT_R32G32B32A32_UINT;
    case Format::r16_float:         return VK_FORMAT_R16_SFLOAT;
    case Format::rg16_float:        return VK_FORMAT_R16G16_SFLOAT;
    case Format::rgba16_float:      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::r32_float:         return VK_FORMAT_R32_SFLOAT;
    case Format::rg32_float:        return VK_FORMAT_R32G32_SFLOAT;
    case Format::rgba32_float:      return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::rgb10a2_unorm:     return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case Format::rg11b10_float:     return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case Format::d16_unorm:         return VK_FORMAT_D16_UNORM;
    case Format::d32_float:         return VK_FORMAT_D32_SFLOAT;
    case Format::d24_unorm_s8_uint: return VK_FORMAT_D24_UNORM_S8_UINT;
    case Format::d32_float_s8_uint: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case Format::bc1_unorm:         return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case Format::bc1_srgb:          return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case Format::bc3_unorm:         return VK_FORMAT_BC3_UNORM_BLOCK;
    case Format::bc3_srgb:          return VK_FORMAT_BC3_SRGB_BLOCK;
    case Format::bc4_unorm:         return VK_FORMAT_BC4_UNORM_BLOCK;
    case Format::bc5_unorm:         return VK_FORMAT_BC5_UNORM_BLOCK;
    case Format::bc6h_ufloat:       return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case Format::bc7_unorm:         return VK_FORMAT_BC7_UNORM_BLOCK;
    case Format::bc7_srgb:          return VK_FORMAT_BC7_SRGB_BLOCK;
    case Format::astc_4x4_unorm:    return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    case Format::astc_4x4_srgb:     return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
    case Format::undefined:         return VK_FORMAT_UNDEFINED;
    }
    return VK_FORMAT_UNDEFINED;
}

[[nodiscard]] constexpr Format format_from_vk(VkFormat format) noexcept
{
    for (uint32 index = 0; index < static_cast<uint32>(Format::undefined); ++index)
    {
        const Format candidate = static_cast<Format>(index);
        if (to_vk(candidate) == format) return candidate;
    }
    return Format::undefined;
}

[[nodiscard]] constexpr VkImageAspectFlags aspect_of(Format format, TextureAspect aspect) noexcept
{
    const FormatInfo info = get_format_info(format);
    switch (aspect)
    {
    case TextureAspect::color:   return VK_IMAGE_ASPECT_COLOR_BIT;
    case TextureAspect::depth:   return VK_IMAGE_ASPECT_DEPTH_BIT;
    case TextureAspect::stencil: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case TextureAspect::automatic:
        break;
    }
    if (info.depth) return VK_IMAGE_ASPECT_DEPTH_BIT;
    if (info.stencil) return VK_IMAGE_ASPECT_STENCIL_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

[[nodiscard]] VkPipelineStageFlags2 to_vk(Stage stages, bool mesh_shaders, bool task_shaders) noexcept
{
    VkPipelineStageFlags2 result = 0;
    if (has_flag(stages, Stage::indirect)) result |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    if (has_flag(stages, Stage::index_input)) result |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
    if (has_flag(stages, Stage::vertex)) result |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    if (has_flag(stages, Stage::task) && task_shaders) result |= VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT;
    if (has_flag(stages, Stage::mesh) && mesh_shaders) result |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
    if (has_flag(stages, Stage::depth_stencil_tests))
        result |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    if (has_flag(stages, Stage::fragment)) result |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    if (has_flag(stages, Stage::color_output)) result |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (has_flag(stages, Stage::compute)) result |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (has_flag(stages, Stage::transfer)) result |= VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
    if (has_flag(stages, Stage::host)) result |= VK_PIPELINE_STAGE_2_HOST_BIT;
    if (has_flag(stages, Stage::all_commands)) result |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    return result;
}

[[nodiscard]] constexpr VkAccessFlags2 to_vk(Access accesses) noexcept
{
    VkAccessFlags2 result = 0;
    if (has_flag(accesses, Access::transfer_read)) result |= VK_ACCESS_2_TRANSFER_READ_BIT;
    if (has_flag(accesses, Access::transfer_write)) result |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (has_flag(accesses, Access::shader_read))
        result |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    if (has_flag(accesses, Access::shader_write)) result |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    if (has_flag(accesses, Access::color_read)) result |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    if (has_flag(accesses, Access::color_write)) result |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    if (has_flag(accesses, Access::depth_stencil_read)) result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    if (has_flag(accesses, Access::depth_stencil_write)) result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (has_flag(accesses, Access::indirect_read)) result |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    if (has_flag(accesses, Access::index_read)) result |= VK_ACCESS_2_INDEX_READ_BIT;
    if (has_flag(accesses, Access::host_read)) result |= VK_ACCESS_2_HOST_READ_BIT;
    return result;
}

[[nodiscard]] constexpr VkFilter to_vk(Filter filter) noexcept
{
    return filter == Filter::linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

[[nodiscard]] constexpr VkSamplerMipmapMode to_mipmap_mode(Filter filter) noexcept
{
    return filter == Filter::linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
}

[[nodiscard]] constexpr VkSamplerAddressMode to_vk(AddressMode mode) noexcept
{
    switch (mode)
    {
    case AddressMode::repeat:          return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case AddressMode::mirrored_repeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case AddressMode::clamp_to_edge:   return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case AddressMode::clamp_to_border: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

[[nodiscard]] constexpr VkCompareOp to_vk(CompareOp op) noexcept
{
    switch (op)
    {
    case CompareOp::never:         return VK_COMPARE_OP_NEVER;
    case CompareOp::less:          return VK_COMPARE_OP_LESS;
    case CompareOp::equal:         return VK_COMPARE_OP_EQUAL;
    case CompareOp::less_equal:    return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOp::greater:       return VK_COMPARE_OP_GREATER;
    case CompareOp::not_equal:     return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOp::greater_equal: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOp::always:        return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

[[nodiscard]] constexpr VkStencilOp to_vk(StencilOp op) noexcept
{
    switch (op)
    {
    case StencilOp::keep:            return VK_STENCIL_OP_KEEP;
    case StencilOp::zero:            return VK_STENCIL_OP_ZERO;
    case StencilOp::replace:         return VK_STENCIL_OP_REPLACE;
    case StencilOp::increment_clamp: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case StencilOp::decrement_clamp: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case StencilOp::invert:          return VK_STENCIL_OP_INVERT;
    case StencilOp::increment_wrap:  return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case StencilOp::decrement_wrap:  return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_KEEP;
}

[[nodiscard]] constexpr VkBlendFactor to_vk(BlendFactor factor) noexcept
{
    switch (factor)
    {
    case BlendFactor::zero:                        return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::one:                         return VK_BLEND_FACTOR_ONE;
    case BlendFactor::source_color:                return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::one_minus_source_color:      return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::destination_color:           return VK_BLEND_FACTOR_DST_COLOR;
    case BlendFactor::one_minus_destination_color: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::source_alpha:                return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::one_minus_source_alpha:      return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::destination_alpha:           return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::one_minus_destination_alpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case BlendFactor::source_alpha_saturate:       return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    }
    return VK_BLEND_FACTOR_ONE;
}

[[nodiscard]] constexpr VkBlendOp to_vk(BlendOp op) noexcept
{
    switch (op)
    {
    case BlendOp::add:              return VK_BLEND_OP_ADD;
    case BlendOp::subtract:         return VK_BLEND_OP_SUBTRACT;
    case BlendOp::reverse_subtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case BlendOp::minimum:          return VK_BLEND_OP_MIN;
    case BlendOp::maximum:          return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

[[nodiscard]] constexpr VkPrimitiveTopology to_vk(Topology topology) noexcept
{
    switch (topology)
    {
    case Topology::points:         return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case Topology::lines:          return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case Topology::line_strip:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case Topology::triangles:      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case Topology::triangle_strip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

[[nodiscard]] constexpr VkAttachmentLoadOp to_vk(LoadOp op) noexcept
{
    switch (op)
    {
    case LoadOp::load:    return VK_ATTACHMENT_LOAD_OP_LOAD;
    case LoadOp::clear:   return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case LoadOp::discard: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_LOAD;
}

[[nodiscard]] constexpr VkAttachmentStoreOp to_vk(StoreOp op) noexcept
{
    return op == StoreOp::store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

// ---------------------------------------------------------------------------------------------
// 3. Slot allocator
// ---------------------------------------------------------------------------------------------

// TODO: make benchmark comparing other slot allocator strategie
struct SlotAllocator
{
    std::vector<uint32> free_list;
    uint32 capacity = 0;
    uint32 next     = 0;

    void initialize(uint32 slot_capacity)
    {
        free_list.reserve(slot_capacity);
        capacity = slot_capacity;
        next = 0;
    }

    [[nodiscard]] uint32 allocate() noexcept
    {
        if (!free_list.empty())
        {
            const uint32 slot = free_list.back();
            free_list.pop_back();
            return slot;
        }
        if (next < capacity) return next++;
        return invalid_handle;
    }

    void release(uint32 slot) noexcept
    {
        if (slot == invalid_handle || free_list.size() >= capacity) return;
        free_list.push_back(slot);
    }
};

} // namespace

// ---------------------------------------------------------------------------------------------
// 4. Object definitions
// ---------------------------------------------------------------------------------------------

struct TimelineSemaphore
{
    Device* device = nullptr;
    VkSemaphore semaphore = VK_NULL_HANDLE;
};

struct Heap
{
    Device* device = nullptr;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64 size = 0;
    MemoryType type = MemoryType::gpu_only;
    byte* mapped = nullptr; // Mapped once at creation; never remapped, never unmapped early.
};

struct TextureHeap
{
    Device* device = nullptr;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64 size = 0;
};

struct Buffer
{
    Device* device = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory owned_memory = VK_NULL_HANDLE; // Null when placed in a heap.
    uint64 size = 0;
    byte* mapped = nullptr;
    MemoryType memory_type = MemoryType::gpu_only;
};

struct Texture
{
    Device* device = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory owned_memory = VK_NULL_HANDLE;
    TextureDesc desc = {};
    bool owns_image = true; // False for swapchain images.
};

struct BufferView
{
    Device* device = nullptr;
    VkBufferView texel_view = VK_NULL_HANDLE; // Only the texel view types create one.
    ResourceHandle slot = invalid_handle;
};

struct TextureView
{
    Device* device = nullptr;
    VkImageView image_view = VK_NULL_HANDLE;
    ResourceHandle slot = invalid_handle;
};

struct Sampler
{
    Device* device = nullptr;
    VkSampler sampler = VK_NULL_HANDLE;
    ResourceHandle slot = invalid_handle;
};

struct RenderView
{
    Device* device = nullptr;
    VkImageView image_view = VK_NULL_HANDLE;
    Format format = Format::undefined;
    uint32x2 extent = {};
    bool depth = false;
    bool stencil = false;
};

struct PSO
{
    Device* device = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
};

// Where one resolved timestamp is written once the command buffer ends.
struct TimestampTarget
{
    VkBuffer buffer = VK_NULL_HANDLE;
    uint64 offset = 0;
};

struct CommandBuffer
{
    Device* device = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    uint64 retire_value = 0; // Internal timeline value that must pass before this is recycled.
    bool recording = false;
    bool in_render_pass = false;
    uint32x2 render_area = {};

    VkQueryPool query_pool = VK_NULL_HANDLE;
    std::vector<TimestampTarget> timestamp_targets;
    uint32 timestamp_count = 0;
};

struct Device
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32 queue_family = 0;
    VkPhysicalDeviceMemoryProperties memory_properties = {};
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

    DeviceDesc desc = {};
    DeviceCaps caps = {};
    Error error = Error::none;

    // Global bindless set: binding 0 mutable resources, binding 1 samplers.
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    SlotAllocator resource_slots = {};
    SlotAllocator sampler_slots = {};

    // One memory type valid for every image wvk can create, resolved once at device creation.
    uint32 texture_memory_type = UINT32_MAX;

    // Internal frame timeline, distinct from any application semaphore.
    VkSemaphore frame_timeline = VK_NULL_HANDLE;
    uint64 frame_counter = 0;

    // Command buffer ring.
    std::vector<CommandBuffer*> command_buffers;

    // Textures awaiting their one-time UNDEFINED -> GENERAL transition.
    std::vector<Texture*> pending_textures;

    // Presentation.
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    uint32x2 swapchain_extent = {};
    uint32x2 drawable_hint = {}; // Application-supplied pixel size; the only size Wayland gives us.
    uint32 swapchain_image_count = 0;
    uint32 image_index = 0;
    uint32 frame_index = 0;
    bool frame_acquired = false;
    bool swapchain_dirty = false;
    Texture* swapchain_textures[max_swapchain_images] = {};
    RenderView* swapchain_views[max_swapchain_images] = {};
    bool swapchain_initialized[max_swapchain_images] = {};
    VkSemaphore present_semaphores[max_swapchain_images] = {};
    VkSemaphore acquire_semaphores[max_swapchain_images] = {};
    uint64 frame_retire_values[max_swapchain_images] = {};
    VkCommandPool frame_pool = VK_NULL_HANDLE;
    VkCommandBuffer pre_present[max_swapchain_images] = {};
    VkCommandBuffer post_present[max_swapchain_images] = {};

    PFN_vkCmdDrawMeshTasksEXT cmd_draw_mesh_tasks = nullptr;
    PFN_vkCmdDrawMeshTasksIndirectEXT cmd_draw_mesh_tasks_indirect = nullptr;
    PFN_vkCmdDrawMeshTasksIndirectCountEXT cmd_draw_mesh_tasks_indirect_count = nullptr;
};

namespace
{

void fail(Device* device, Error error) noexcept
{
    if (device && device->error == Error::none) device->error = error;
}

[[nodiscard]] bool check(Device* device, VkResult result) noexcept
{
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) return true;
    fail(device, error_from_vk(result));
    return false;
}

[[nodiscard]] bool alive(const Device* device) noexcept
{
    return device && device->error != Error::device_lost;
}

[[nodiscard]] uint32 find_memory_type(const Device* device, uint32 type_bits, VkMemoryPropertyFlags required) noexcept
{
    for (uint32 index = 0; index < device->memory_properties.memoryTypeCount; ++index)
    {
        if ((type_bits & (1u << index)) == 0) continue;
        if ((device->memory_properties.memoryTypes[index].propertyFlags & required) == required) return index;
    }
    return UINT32_MAX;
}

[[nodiscard]] VkMemoryPropertyFlags properties_of(MemoryType type) noexcept
{
    switch (type)
    {
    case MemoryType::gpu_only:
        return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    case MemoryType::cpu_to_gpu:
        return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    case MemoryType::gpu_to_cpu:
        return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
               VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    }
    return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
}

// HOST_CACHED is not universally exposed; readback correctness only needs HOST_VISIBLE|HOST_COHERENT,
// so a device without a cached type falls back rather than failing outright.
[[nodiscard]] uint32 select_memory_type(const Device* device, uint32 type_bits, MemoryType type) noexcept
{
    const VkMemoryPropertyFlags required = properties_of(type);
    uint32 index = find_memory_type(device, type_bits, required);
    if (index == UINT32_MAX && (required & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0)
    {
        index = find_memory_type(device, type_bits, required & ~VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    }
    if (index == UINT32_MAX && type == MemoryType::gpu_only)
    {
        index = find_memory_type(device, type_bits, 0);
    }
    return index;
}

// Every buffer is transfer-, indirect- and address-capable, and usable as a storage buffer. Vulkan
// demands these be requested up front where D3D12 and Metal infer them, and the uniform cost keeps
// the descriptor model free of per-buffer special cases.
[[nodiscard]] VkBufferUsageFlags usage_of(BufferUsage usage) noexcept
{
    VkBufferUsageFlags flags = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (has_flag(usage, BufferUsage::index)) flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (has_flag(usage, BufferUsage::uniform_texel)) flags |= VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
    if (has_flag(usage, BufferUsage::storage_texel)) flags |= VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    return flags;
}

// Widening this to always include the transfer bits, the way an API
// mirroring D3D12's implicit copy support would, costs framebuffer compression on hardware that
// keys compression off the usage set.
[[nodiscard]] VkImageUsageFlags usage_of(TextureUsage usage) noexcept
{
    VkImageUsageFlags flags = 0;
    if (has_flag(usage, TextureUsage::transfer_source)) flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (has_flag(usage, TextureUsage::transfer_destination)) flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (has_flag(usage, TextureUsage::sampled)) flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (has_flag(usage, TextureUsage::storage)) flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (has_flag(usage, TextureUsage::color_attachment)) flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (has_flag(usage, TextureUsage::depth_stencil_attachment)) flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    return flags;
}

[[nodiscard]] VkImageCreateInfo image_info_of(const TextureDesc& desc) noexcept
{
    VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = desc.type == TextureType::one_d     ? VK_IMAGE_TYPE_1D
                     : desc.type == TextureType::three_d ? VK_IMAGE_TYPE_3D
                                                         : VK_IMAGE_TYPE_2D;
    info.format = to_vk(desc.format);
    info.extent = {desc.extent.x, maximum(desc.extent.y, 1u),
                   desc.type == TextureType::three_d ? maximum(desc.extent.z, 1u) : 1u};
    info.mipLevels = maximum(desc.mip_levels, 1u);
    info.arrayLayers = desc.type == TextureType::three_d ? 1u : maximum(desc.layer_count, 1u);
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage_of(desc.usage);
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.flags = desc.mutable_format ? VkImageCreateFlags{VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT} : VkImageCreateFlags{0};
    if (desc.type == TextureType::cube) info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    return info;
}

[[nodiscard]] uint64 resolved_range(const Buffer* buffer, uint64 offset, uint64 size) noexcept
{
    if (size != 0) return size;
    return buffer && buffer->size > offset ? buffer->size - offset : 0;
}

// ---------------------------------------------------------------------------------------------
// 5. Device creation helpers
// ---------------------------------------------------------------------------------------------

struct RequiredFeatures
{
    VkPhysicalDeviceFeatures2 core{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features v11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features v12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features v13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutable_type{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};

    void chain(bool include_mesh) noexcept
    {
        core.pNext = &v11;
        v11.pNext = &v12;
        v12.pNext = &v13;
        v13.pNext = &mutable_type;
        mutable_type.pNext = include_mesh ? static_cast<void*>(&mesh) : nullptr;
        mesh.pNext = nullptr;
    }
};

[[nodiscard]] bool has_required_features(const RequiredFeatures& f) noexcept
{
    return f.core.features.shaderStorageImageWriteWithoutFormat &&
        //f.core.features.shaderStorageImageReadWithoutFormat &&
           f.v11.shaderDrawParameters &&
           f.v12.timelineSemaphore &&
           f.v12.bufferDeviceAddress &&
           f.v12.scalarBlockLayout &&
           f.v12.runtimeDescriptorArray &&
           f.v12.descriptorIndexing &&
           f.v12.shaderSampledImageArrayNonUniformIndexing &&
           f.v12.shaderStorageImageArrayNonUniformIndexing &&
           f.v12.shaderStorageBufferArrayNonUniformIndexing &&
           f.v12.shaderUniformTexelBufferArrayNonUniformIndexing &&
           f.v12.shaderStorageTexelBufferArrayNonUniformIndexing &&
           f.v12.descriptorBindingSampledImageUpdateAfterBind &&
           f.v12.descriptorBindingStorageImageUpdateAfterBind &&
           f.v12.descriptorBindingStorageBufferUpdateAfterBind &&
           f.v12.descriptorBindingUniformTexelBufferUpdateAfterBind &&
           f.v12.descriptorBindingStorageTexelBufferUpdateAfterBind &&
           f.v12.descriptorBindingPartiallyBound &&
           f.v12.descriptorBindingUpdateUnusedWhilePending &&
           f.v13.synchronization2 &&
           f.v13.dynamicRendering &&
           f.v13.maintenance4 &&
           f.mutable_type.mutableDescriptorType;
}

void enable_required_features(RequiredFeatures& f) noexcept
{
    // Everything not listed is left at whatever the query returned; the device is created from a
    // filtered copy, so only the bits set here are actually requested.
    RequiredFeatures enabled;
    enabled.core.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    enabled.core.features.shaderStorageImageReadWithoutFormat = f.core.features.shaderStorageImageReadWithoutFormat;
    enabled.core.features.samplerAnisotropy = f.core.features.samplerAnisotropy;
    enabled.core.features.multiDrawIndirect = f.core.features.multiDrawIndirect;
    enabled.core.features.drawIndirectFirstInstance = f.core.features.drawIndirectFirstInstance;
    enabled.core.features.depthClamp = f.core.features.depthClamp;
    enabled.core.features.fillModeNonSolid = f.core.features.fillModeNonSolid;
    enabled.core.features.independentBlend = f.core.features.independentBlend;
    enabled.core.features.shaderInt16 = f.core.features.shaderInt16;
    enabled.core.features.textureCompressionBC = f.core.features.textureCompressionBC;
    enabled.core.features.textureCompressionASTC_LDR = f.core.features.textureCompressionASTC_LDR;

    enabled.v11.shaderDrawParameters = VK_TRUE;
    enabled.v11.storageBuffer16BitAccess = f.v11.storageBuffer16BitAccess;

    enabled.v12.timelineSemaphore = VK_TRUE;
    enabled.v12.bufferDeviceAddress = VK_TRUE;
    enabled.v12.scalarBlockLayout = VK_TRUE;
    enabled.v12.runtimeDescriptorArray = VK_TRUE;
    enabled.v12.descriptorIndexing = VK_TRUE;
    enabled.v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    enabled.v12.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
    enabled.v12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
    enabled.v12.shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE;
    enabled.v12.shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE;
    enabled.v12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    enabled.v12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    enabled.v12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    enabled.v12.descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE;
    enabled.v12.descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
    enabled.v12.descriptorBindingPartiallyBound = VK_TRUE;
    enabled.v12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    enabled.v12.drawIndirectCount = f.v12.drawIndirectCount;
    enabled.v12.shaderFloat16 = f.v12.shaderFloat16;

    enabled.v13.synchronization2 = VK_TRUE;
    enabled.v13.dynamicRendering = VK_TRUE;
    enabled.v13.maintenance4 = VK_TRUE;

    enabled.mutable_type.mutableDescriptorType = VK_TRUE;
    enabled.mesh.meshShader = f.mesh.meshShader;
    enabled.mesh.taskShader = f.mesh.taskShader;

    f = enabled;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void*)
{
    std::string level;
    if      (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT == severity)   level = "[ERROR  ] ";
    else if (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT == severity) level = "[WARNING] ";
    else if (VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT == severity)    level = "[WARNING] ";
    else level = "[DEBUG  ] ";

    std::cout << level << callbackData->pMessage << std::endl;
    return VK_FALSE;
}

// The five descriptor types binding 0 accepts. UNIFORM_BUFFER is intentionally absent: nothing in
// shaders/wvk.hlsl consumes one, and omitting it removes the requirement on
// descriptorBindingUniformBufferUpdateAfterBind, the weakest link in the update-after-bind set.
constexpr VkDescriptorType mutable_descriptor_types[]{
    VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
    VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
};

[[nodiscard]] bool create_descriptor_heap(Device* device, uint32 resource_capacity, uint32 sampler_capacity) noexcept
{
    VkMutableDescriptorTypeListEXT lists[2]{};
    lists[0].descriptorTypeCount = static_cast<uint32>(sizeof(mutable_descriptor_types) / sizeof(mutable_descriptor_types[0]));
    lists[0].pDescriptorTypes = mutable_descriptor_types;
    // Binding 1 is a plain sampler binding; its entry exists only to keep the array indices aligned
    // with pBindings, as the extension requires.
    lists[1] = {};

    const VkMutableDescriptorTypeCreateInfoEXT mutable_info{
        .sType = VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT,
        .mutableDescriptorTypeListCount = 2,
        .pMutableDescriptorTypeLists = lists,
    };

    const VkDescriptorSetLayoutBinding bindings[2]{
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_MUTABLE_EXT,
            .descriptorCount = resource_capacity,
            .stageFlags = VK_SHADER_STAGE_ALL,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .descriptorCount = sampler_capacity,
            .stageFlags = VK_SHADER_STAGE_ALL,
        },
    };

    const VkDescriptorBindingFlags binding_flags[2]{
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
    };

    const VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .pNext = &mutable_info,
        .bindingCount = 2,
        .pBindingFlags = binding_flags,
    };

    const VkDescriptorSetLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &flags_info,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
        .bindingCount = 2,
        .pBindings = bindings,
    };
    if (!check(device, vkCreateDescriptorSetLayout(device->device, &layout_info, nullptr, &device->set_layout)))
        return false;

    const VkDescriptorPoolSize pool_sizes[2]{
        {VK_DESCRIPTOR_TYPE_MUTABLE_EXT, resource_capacity},
        {VK_DESCRIPTOR_TYPE_SAMPLER, sampler_capacity},
    };
    // Chaining the same type list to the pool lets the driver size the mutable slots to the widest
    // descriptor in the list instead of the widest descriptor it supports at all.
    const VkDescriptorPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = &mutable_info,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets = 1,
        .poolSizeCount = 2,
        .pPoolSizes = pool_sizes,
    };
    if (!check(device, vkCreateDescriptorPool(device->device, &pool_info, nullptr, &device->descriptor_pool)))
        return false;

    const VkDescriptorSetAllocateInfo allocate_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = device->descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &device->set_layout,
    };
    if (!check(device, vkAllocateDescriptorSets(device->device, &allocate_info, &device->descriptor_set)))
        return false;

    const VkPushConstantRange push_range{
        .stageFlags = VK_SHADER_STAGE_ALL,
        .offset = 0,
        .size = device->caps.max_push_constant_bytes,
    };
    const VkPipelineLayoutCreateInfo pipeline_layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &device->set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    };
    return check(device, vkCreatePipelineLayout(device->device, &pipeline_layout_info, nullptr, &device->pipeline_layout));
}

// Writes one descriptor into binding 0 and returns the slot, which is the value shaders index with.
[[nodiscard]] ResourceHandle write_resource_descriptor(Device* device, VkDescriptorType type,
                                                       const VkDescriptorImageInfo* image,
                                                       const VkDescriptorBufferInfo* buffer,
                                                       const VkBufferView* texel) noexcept
{
    const uint32 slot = device->resource_slots.allocate();
    if (slot == invalid_handle)
    {
        fail(device, Error::heap_exhausted);
        return invalid_handle;
    }
    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = device->descriptor_set,
        .dstBinding = 0,
        .dstArrayElement = slot,
        .descriptorCount = 1,
        .descriptorType = type,
        .pImageInfo = image,
        .pBufferInfo = buffer,
        .pTexelBufferView = texel,
    };
    vkUpdateDescriptorSets(device->device, 1, &write, 0, nullptr);
    return slot;
}

// Resolves one GPU-only memory type usable by every image wvk can create, plus a heap alignment
// that satisfies all of them. An image's memoryTypeBits can be strictly narrower than a buffer's,
// and narrower still for depth/stencil, so a texture heap picked from a buffer's requirements would
// bind for some textures and fail for others. Intersecting a representative set up front turns that
// into a single device-wide answer, which is why create_texture_heap takes no memory type.
bool probe_image(Device* device, VkImageCreateInfo info, uint32& type_bits, uint64& alignment) noexcept
{
    VkImageFormatProperties supported{};
    if (vkGetPhysicalDeviceImageFormatProperties(device->physical_device, info.format, info.imageType, info.tiling,
                                                 info.usage, info.flags, &supported) != VK_SUCCESS)
        return false;
    if (info.extent.width > supported.maxExtent.width || info.extent.height > supported.maxExtent.height ||
        info.extent.depth > supported.maxExtent.depth || info.mipLevels > supported.maxMipLevels ||
        info.arrayLayers > supported.maxArrayLayers)
        return false;

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(device->device, &info, nullptr, &image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device->device, image, &requirements);
    vkDestroyImage(device->device, image, nullptr);

    type_bits &= requirements.memoryTypeBits;
    alignment = maximum(alignment, requirements.alignment);
    return true;
}

[[nodiscard]] bool select_texture_memory_type(Device* device) noexcept
{
    uint32 type_bits = ~0u;
    uint64 alignment = 1;

    VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {1024, 1024, 1};
    info.mipLevels = 11;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!probe_image(device, info, type_bits, alignment))
    {
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!probe_image(device, info, type_bits, alignment)) return false;
    }

    // The same image again with a mutable format, since that flag can move an image onto a
    // different memory type on some drivers.
    VkImageCreateInfo mutable_info = info;
    mutable_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    probe_image(device, mutable_info, type_bits, alignment);

    VkImageCreateInfo cube_info = info;
    cube_info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    cube_info.extent = {512, 512, 1};
    cube_info.mipLevels = 10;
    cube_info.arrayLayers = 6;
    probe_image(device, cube_info, type_bits, alignment);

    VkImageCreateInfo volume_info = info;
    volume_info.imageType = VK_IMAGE_TYPE_3D;
    volume_info.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    volume_info.extent = {256, 256, 64};
    volume_info.mipLevels = 1;
    volume_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    probe_image(device, volume_info, type_bits, alignment);

    constexpr Format compressed_formats[]{Format::bc7_unorm, Format::astc_4x4_unorm};
    for (Format format : compressed_formats)
    {
        VkImageCreateInfo compressed_info = info;
        compressed_info.format = to_vk(format);
        compressed_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        probe_image(device, compressed_info, type_bits, alignment);
    }

    // Depth and stencil are the formats most likely to sit on a narrower memory type.
    constexpr Format depth_formats[]{Format::d16_unorm, Format::d32_float, Format::d24_unorm_s8_uint,
                                     Format::d32_float_s8_uint};
    for (Format format : depth_formats)
    {
        VkImageCreateInfo depth_info = info;
        depth_info.format = to_vk(format);
        depth_info.mipLevels = 1;
        depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!probe_image(device, depth_info, type_bits, alignment))
        {
            depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            probe_image(device, depth_info, type_bits, alignment);
        }
    }

    if (type_bits == 0) return false;
    device->texture_memory_type = find_memory_type(device, type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (device->texture_memory_type == UINT32_MAX) device->texture_memory_type = find_memory_type(device, type_bits, 0);
    device->caps.texture_heap_alignment = alignment;
    return device->texture_memory_type != UINT32_MAX;
}

void destroy_swapchain_objects(Device* device) noexcept
{
    for (uint32 index = 0; index < device->swapchain_image_count; ++index)
    {
        if (device->swapchain_views[index])
        {
            vkDestroyImageView(device->device, device->swapchain_views[index]->image_view, nullptr);
            destroy_object(device->swapchain_views[index]);
            device->swapchain_views[index] = nullptr;
        }
        destroy_object(device->swapchain_textures[index]);
        device->swapchain_textures[index] = nullptr;
        device->swapchain_initialized[index] = false;
    }
    device->swapchain_image_count = 0;
}

[[nodiscard]] bool create_swapchain(Device* device) noexcept
{
    VkSurfaceCapabilitiesKHR capabilities{};
    if (!check(device, vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->physical_device, device->surface, &capabilities)))
        return false;

    // Wayland reports currentExtent as 0xFFFFFFFF on both axes: the compositor does not dictate a
    // size, the surface adopts whatever the swapchain asks for. Passing that value straight through
    // makes vkCreateSwapchainKHR fail, so the application-supplied drawable extent takes over.
    constexpr uint32 undefined_extent = 0xffffffffu;
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == undefined_extent || extent.height == undefined_extent)
    {
        extent.width = device->drawable_hint.x;
        extent.height = device->drawable_hint.y;
    }
    if (extent.width == 0 || extent.height == 0)
    {
        device->swapchain_extent = {};
        return true; // Minimized, or no extent supplied yet; try again next acquire.
    }
    extent.width = minimum(maximum(extent.width, capabilities.minImageExtent.width), capabilities.maxImageExtent.width);
    extent.height =
        minimum(maximum(extent.height, capabilities.minImageExtent.height), capabilities.maxImageExtent.height);

    // OPAQUE is not universal. Some Wayland compositors expose only INHERIT, and creating a
    // swapchain with an unsupported composite alpha mode is invalid.
    VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (const VkCompositeAlphaFlagBitsKHR candidate :
         {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
    {
        if ((capabilities.supportedCompositeAlpha & candidate) != 0)
        {
            composite_alpha = candidate;
            break;
        }
    }

    uint32 image_count = maximum(device->desc.desired_swapchain_image_count, capabilities.minImageCount);
    if (capabilities.maxImageCount != 0) image_count = minimum(image_count, capabilities.maxImageCount);
    image_count = minimum(image_count, max_swapchain_images);

    // STORAGE on a presented image can disable display compression, so it is opt-in and only granted
    // when the surface actually reports it.
    VkImageUsageFlags swapchain_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    device->caps.swapchain_storage = false;
    if (device->desc.request_swapchain_storage &&
        (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT) != 0)
    {
        swapchain_usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        device->caps.swapchain_storage = true;
    }
    swapchain_usage &= capabilities.supportedUsageFlags;

    const VkSwapchainKHR previous = device->swapchain;
    const VkSwapchainCreateInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = device->surface,
        .minImageCount = image_count,
        .imageFormat = device->swapchain_format,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = swapchain_usage,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = composite_alpha,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = previous,
    };

    VkSwapchainKHR created = VK_NULL_HANDLE;
    if (!check(device, vkCreateSwapchainKHR(device->device, &info, nullptr, &created))) return false;

    destroy_swapchain_objects(device);
    if (previous) vkDestroySwapchainKHR(device->device, previous, nullptr);
    device->swapchain = created;
    device->swapchain_extent = {extent.width, extent.height};

    // Two-call form. A driver may hand back more images than minImageCount asked for, and passing a
    // fixed-size array would return VK_INCOMPLETE, which reads as a failure.
    uint32 actual = 0;
    if (!check(device, vkGetSwapchainImagesKHR(device->device, created, &actual, nullptr))) return false;
    if (actual > max_swapchain_images)
    {
        fail(device, Error::unsupported);
        return false;
    }
    VkImage images[max_swapchain_images]{};
    if (!check(device, vkGetSwapchainImagesKHR(device->device, created, &actual, images))) return false;
    device->swapchain_image_count = actual;

    for (uint32 index = 0; index < actual; ++index)
    {
        Texture* texture = create_object<Texture>();
        if (!texture)
        {
            fail(device, Error::out_of_memory);
            return false;
        }
        texture->device = device;
        texture->image = images[index];
        texture->owns_image = false;
        texture->desc.type = TextureType::two_d;
        texture->desc.extent = {device->swapchain_extent.x, device->swapchain_extent.y, 1};
        texture->desc.format = device->caps.swapchain_format;
        texture->desc.usage = device->caps.swapchain_storage
                                  ? (TextureUsage::color_attachment | TextureUsage::transfer_destination |
                                     TextureUsage::storage)
                                  : (TextureUsage::color_attachment | TextureUsage::transfer_destination);
        device->swapchain_textures[index] = texture;

        const VkImageViewCreateInfo view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[index],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = device->swapchain_format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        VkImageView view = VK_NULL_HANDLE;
        if (!check(device, vkCreateImageView(device->device, &view_info, nullptr, &view))) return false;

        RenderView* render_view = create_object<RenderView>();
        if (!render_view)
        {
            vkDestroyImageView(device->device, view, nullptr);
            fail(device, Error::out_of_memory);
            return false;
        }
        render_view->device = device;
        render_view->image_view = view;
        render_view->format = device->caps.swapchain_format;
        render_view->extent = device->swapchain_extent;
        device->swapchain_views[index] = render_view;
    }
    return true;
}

// Records the two layout transitions the swapchain still needs. Everything else in wvk avoids image
// barriers entirely; presentation is the one place Vulkan insists on a specific layout.
void record_present_transitions(Device* device, uint32 image) noexcept
{
    const VkCommandBufferBeginInfo begin{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    VkImageMemoryBarrier2 to_general{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = device->swapchain_initialized[image] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                                          : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = device->swapchain_textures[image]->image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                .imageMemoryBarrierCount = 1,
                                .pImageMemoryBarriers = &to_general};
    vkBeginCommandBuffer(device->pre_present[image], &begin);
    vkCmdPipelineBarrier2(device->pre_present[image], &dependency);
    vkEndCommandBuffer(device->pre_present[image]);

    VkImageMemoryBarrier2 to_present = to_general;
    to_present.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    dependency.pImageMemoryBarriers = &to_present;
    vkBeginCommandBuffer(device->post_present[image], &begin);
    vkCmdPipelineBarrier2(device->post_present[image], &dependency);
    vkEndCommandBuffer(device->post_present[image]);

    device->swapchain_initialized[image] = true;
}

void wait_frame_timeline(Device* device, uint64 value) noexcept
{
    if (value == 0) return;
    const VkSemaphoreWaitInfo info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &device->frame_timeline,
        .pValues = &value,
    };
    vkWaitSemaphores(device->device, &info, UINT64_MAX);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// 5. Device creation
// ---------------------------------------------------------------------------------------------

DeviceInit create_device(const DeviceDesc& desc) noexcept
{
    Device* device = create_object<Device>();
    if (!device) return {.error = Error::out_of_memory};
    device->desc = desc;
    device->drawable_hint = desc.drawable_extent;

    const bool windowed = desc.handle != nullptr;

    uint32 loader_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion(&loader_version) != VK_SUCCESS || loader_version < VK_API_VERSION_1_3)
    {
        destroy_object(device);
        return {.error = Error::unsupported};
    }

    VkExtensionProperties instance_extensions[max_enumerated_extensions]{};
    uint32 instance_extension_count = max_enumerated_extensions;
    vkEnumerateInstanceExtensionProperties(nullptr, &instance_extension_count, instance_extensions);

    const char* enabled_instance_extensions[4]{};
    uint32 enabled_instance_extension_count = 0;
    if (windowed)
    {
#if defined(_WIN32)
        const char* platform_surface = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#else
        // Only the extension matching the requested display server is enabled, so a Wayland-only
        // session does not need the X11 one to be present and vice versa.
        const char* platform_surface = surface_extension_of(desc.display_server);
#endif
        if (!has_extension(instance_extensions, instance_extension_count, VK_KHR_SURFACE_EXTENSION_NAME) ||
            !has_extension(instance_extensions, instance_extension_count, platform_surface))
        {
            destroy_object(device);
            return {.error = Error::unsupported};
        }
        enabled_instance_extensions[enabled_instance_extension_count++] = VK_KHR_SURFACE_EXTENSION_NAME;
        enabled_instance_extensions[enabled_instance_extension_count++] = platform_surface;
    }

    const bool debug_utils =
        desc.enable_validation && has_extension(instance_extensions, instance_extension_count, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debug_utils) enabled_instance_extensions[enabled_instance_extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;

    const char* validation_layer = "VK_LAYER_KHRONOS_validation";

    VkValidationFeatureEnableEXT validation_feature_storage[] = {
        VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT,
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
    };
    const VkValidationFeaturesEXT validation_features{
        .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        .enabledValidationFeatureCount = 2,
        .pEnabledValidationFeatures = validation_feature_storage,
    };
    const VkDebugUtilsMessengerCreateInfoEXT messenger_info{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = desc.enable_validation ? &validation_features : nullptr,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debug_callback,
    };

    const void* instance_pnext = nullptr;
    if (debug_utils) instance_pnext = &messenger_info;
    else if (desc.enable_validation) instance_pnext = &validation_features;

    const VkApplicationInfo application{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "wvk application",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .pEngineName = "wvk",
        .engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo instance_info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = instance_pnext,
        .pApplicationInfo = &application,
        .enabledLayerCount = desc.enable_validation ? 1u : 0u,
        .ppEnabledLayerNames = desc.enable_validation ? &validation_layer : nullptr,
        .enabledExtensionCount = enabled_instance_extension_count,
        .ppEnabledExtensionNames = enabled_instance_extensions,
    };
    if (VkResult result = vkCreateInstance(&instance_info, nullptr, &device->instance); result != VK_SUCCESS)
    {
        const Error error = error_from_vk(result);
        destroy_object(device);
        return {.error = error};
    }

    if (debug_utils)
    {
        if (auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(device->instance, "vkCreateDebugUtilsMessengerEXT")))
        {
            VkDebugUtilsMessengerCreateInfoEXT persistent = messenger_info;
            persistent.pNext = nullptr;
            create_messenger(device->instance, &persistent, nullptr, &device->messenger);
        }
    }

    //if (debug_utils)
    //{
    //    std::cout << "WVK Debug enabled" << std::endl;
    //    if (auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
    //            vkGetInstanceProcAddr(device->instance, "vkCreateDebugUtilsMessengerEXT")))
    //    {
    //        VkValidationFeatureEnableEXT validation_feature_storage[] = {
    //            VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT,
    //            VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
    //        };

    //        VkValidationFeaturesEXT validation_features {
    //            .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
    //            .enabledValidationFeatureCount = 2,
    //            .pEnabledValidationFeatures = validation_feature_storage,
    //        };

    //        const VkDebugUtilsMessengerCreateInfoEXT messenger_info{
    //            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
    //            .pNext = &validation_features,
    //            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
    //                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
    //                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
    //            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
    //                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
    //            .pfnUserCallback = debug_callback,
    //        };
    //        create_messenger(device->instance, &messenger_info, nullptr, &device->messenger);
    //    }
    //}

    if (windowed)
    {
#if defined(_WIN32)
        // DeviceDesc::handle is the HWND itself on Win32; there is no wrapper struct to unpack.
        const VkWin32SurfaceCreateInfoKHR surface_info{
            .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
            .hinstance = GetModuleHandleW(nullptr),
            .hwnd = static_cast<HWND>(desc.handle),
        };
        const VkResult surface_result =
            vkCreateWin32SurfaceKHR(device->instance, &surface_info, nullptr, &device->surface);
#else
        const auto* native = static_cast<const LinuxWindowHandle*>(desc.handle);
        const auto create_surface = reinterpret_cast<PFN_CreatePlatformSurface>(
            vkGetInstanceProcAddr(device->instance, surface_entry_point_of(desc.display_server)));
        if (!create_surface)
        {
            destroy_device(device);
            return {.error = Error::unsupported};
        }

        XlibSurfaceCreateInfo xlib{};
        XcbSurfaceCreateInfo xcb{};
        WaylandSurfaceCreateInfo wayland{};
        const void* surface_info = nullptr;
        switch (desc.display_server)
        {
        case DisplayServer::x11:
            xlib = {structure_type_xlib_surface, nullptr, 0, native->display,
                    static_cast<unsigned long>(native->window)};
            surface_info = &xlib;
            break;
        case DisplayServer::xcb:
            xcb = {structure_type_xcb_surface, nullptr, 0, native->display, static_cast<uint32>(native->window)};
            surface_info = &xcb;
            break;
        case DisplayServer::wayland:
            // The wl_surface* travelled here as an integer, so it goes back through uintptr.
            wayland = {structure_type_wayland_surface, nullptr, 0, native->display,
                       reinterpret_cast<void*>(static_cast<uintptr>(native->window))};
            surface_info = &wayland;
            break;
        }
        const VkResult surface_result = create_surface(device->instance, surface_info, nullptr, &device->surface);
#endif
        if (surface_result != VK_SUCCESS)
        {
            const Error error = error_from_vk(surface_result);
            destroy_device(device);
            return {.error = error};
        }
    }

    // Physical device selection.
    VkPhysicalDevice physical_devices[max_physical_devices]{};
    uint32 physical_device_count = max_physical_devices;
    vkEnumeratePhysicalDevices(device->instance, &physical_device_count, physical_devices);

    VkPhysicalDevice selected = VK_NULL_HANDLE;
    uint32 selected_family = 0;
    bool selected_mesh = false;
    RequiredFeatures selected_features;
    VkPhysicalDeviceProperties selected_properties{};
    VkPhysicalDeviceDescriptorIndexingProperties selected_indexing{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};

    for (uint32 candidate = 0; candidate < physical_device_count; ++candidate)
    {
        VkPhysicalDevice physical = physical_devices[candidate];

        VkPhysicalDeviceDescriptorIndexingProperties indexing{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                               .pNext = &indexing};
        vkGetPhysicalDeviceProperties2(physical, &properties);
        if (properties.properties.apiVersion < VK_API_VERSION_1_3) continue;

        VkExtensionProperties device_extensions[max_enumerated_extensions]{};
        uint32 device_extension_count = max_enumerated_extensions;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &device_extension_count, device_extensions);

        if (!has_extension(device_extensions, device_extension_count, VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME))
            continue;
        if (windowed && !has_extension(device_extensions, device_extension_count, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            continue;

        const bool mesh = has_extension(device_extensions, device_extension_count, VK_EXT_MESH_SHADER_EXTENSION_NAME);

        RequiredFeatures features;
        features.chain(mesh);
        vkGetPhysicalDeviceFeatures2(physical, &features.core);
        if (!has_required_features(features)) continue;

        // One queue family for everything, exactly as NoGraphicsAPI does: graphics implies transfer,
        // and a single family removes queue ownership transfers from the barrier model entirely.
        VkQueueFamilyProperties families[max_queue_families]{};
        uint32 family_count = max_queue_families;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);

        uint32 family = UINT32_MAX;
        for (uint32 index = 0; index < family_count; ++index)
        {
            const VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((families[index].queueFlags & required) != required) continue;
            if (windowed)
            {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(physical, index, device->surface, &present);
                if (!present) continue;
            }
            family = index;
            break;
        }
        if (family == UINT32_MAX) continue;

        const bool discrete = properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        const bool better = selected == VK_NULL_HANDLE ||
                            (discrete && selected_properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
        if (!better) continue;

        selected = physical;
        selected_family = family;
        selected_mesh = mesh;
        selected_features = features;
        selected_properties = properties.properties;
        selected_indexing = indexing;
    }

    if (selected == VK_NULL_HANDLE)
    {
        destroy_device(device);
        return {.error = Error::unsupported};
    }

    device->physical_device = selected;
    device->queue_family = selected_family;
    vkGetPhysicalDeviceMemoryProperties(selected, &device->memory_properties);

    // Capabilities are read before the device exists so the descriptor heap can be sized against
    // the real update-after-bind limits rather than a hopeful constant.
    device->caps.device_name = selected_properties.deviceName;
    device->caps.max_push_constant_bytes =
        minimum<uint32>(selected_properties.limits.maxPushConstantsSize, max_push_constant_bytes);
    device->caps.max_color_attachments = minimum(selected_properties.limits.maxColorAttachments, max_color_attachments);
    device->caps.buffer_offset_alignment = selected_properties.limits.minStorageBufferOffsetAlignment;
    device->caps.texel_buffer_offset_alignment = selected_properties.limits.minTexelBufferOffsetAlignment;
    device->caps.max_draw_indirect_count = selected_properties.limits.maxDrawIndirectCount;
    device->caps.max_anisotropy = selected_properties.limits.maxSamplerAnisotropy;
    device->caps.timestamp_period_ns = selected_properties.limits.timestampPeriod;
    // Buffer heaps never hold images, so bufferImageGranularity does not enter into it; what matters
    // is that every buffer alignment the driver can ask for divides this element size.
    device->caps.heap_alignment =
        maximum<uint64>(maximum(selected_properties.limits.minStorageBufferOffsetAlignment,
                                selected_properties.limits.minTexelBufferOffsetAlignment),
                        selected_properties.limits.minMemoryMapAlignment);
    device->caps.mesh_shaders = selected_mesh && selected_features.mesh.meshShader == VK_TRUE;
    device->caps.task_shaders = device->caps.mesh_shaders && selected_features.mesh.taskShader == VK_TRUE;
    device->caps.multi_draw_indirect = selected_features.core.features.multiDrawIndirect == VK_TRUE;
    device->caps.draw_indirect_count = selected_features.v12.drawIndirectCount == VK_TRUE;
    device->caps.depth_clamp = selected_features.core.features.depthClamp == VK_TRUE;
    device->caps.fill_mode_non_solid = selected_features.core.features.fillModeNonSolid == VK_TRUE;
    device->caps.texture_compression_bc = selected_features.core.features.textureCompressionBC == VK_TRUE;
    device->caps.texture_compression_astc = selected_features.core.features.textureCompressionASTC_LDR == VK_TRUE;

    // Binding 0 holds five different descriptor types, so its capacity is bounded by the tightest
    // per-type update-after-bind limit, both per-set and per-stage. Hard-coding a round number here
    // is the classic way to fail on the first driver that reports a smaller ceiling.
    uint32 resource_capacity = device->desc.max_resource_descriptors;
    resource_capacity = minimum(resource_capacity, selected_indexing.maxDescriptorSetUpdateAfterBindSampledImages);
    resource_capacity = minimum(resource_capacity, selected_indexing.maxDescriptorSetUpdateAfterBindStorageImages);
    resource_capacity = minimum(resource_capacity, selected_indexing.maxDescriptorSetUpdateAfterBindStorageBuffers);
    resource_capacity = minimum(resource_capacity, selected_indexing.maxPerStageDescriptorUpdateAfterBindSampledImages);
    resource_capacity = minimum(resource_capacity, selected_indexing.maxPerStageDescriptorUpdateAfterBindStorageImages);
    resource_capacity = minimum(resource_capacity, selected_indexing.maxPerStageDescriptorUpdateAfterBindStorageBuffers);
    resource_capacity = minimum(resource_capacity, selected_indexing.maxPerStageUpdateAfterBindResources);

    uint32 sampler_capacity = device->desc.max_sampler_descriptors;
    sampler_capacity = minimum(sampler_capacity, selected_indexing.maxDescriptorSetUpdateAfterBindSamplers);
    sampler_capacity = minimum(sampler_capacity, selected_indexing.maxPerStageDescriptorUpdateAfterBindSamplers);

    if (resource_capacity == 0 || sampler_capacity == 0)
    {
        destroy_device(device);
        return {.error = Error::unsupported};
    }
    device->caps.max_resource_descriptors = resource_capacity;
    device->caps.max_sampler_descriptors = sampler_capacity;

    RequiredFeatures enabled = selected_features;
    enable_required_features(enabled);
    enabled.chain(device->caps.mesh_shaders);

    const char* enabled_device_extensions[3]{};
    uint32 enabled_device_extension_count = 0;
    enabled_device_extensions[enabled_device_extension_count++] = VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME;
    if (windowed) enabled_device_extensions[enabled_device_extension_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (device->caps.mesh_shaders)
        enabled_device_extensions[enabled_device_extension_count++] = VK_EXT_MESH_SHADER_EXTENSION_NAME;

    constexpr float queue_priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = device->queue_family,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    const VkDeviceCreateInfo device_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enabled.core,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = enabled_device_extension_count,
        .ppEnabledExtensionNames = enabled_device_extensions,
    };
    if (VkResult result = vkCreateDevice(selected, &device_info, nullptr, &device->device); result != VK_SUCCESS)
    {
        const Error error = error_from_vk(result);
        destroy_device(device);
        return {.error = error};
    }
    vkGetDeviceQueue(device->device, device->queue_family, 0, &device->queue);

    if (device->caps.mesh_shaders)
    {
        device->cmd_draw_mesh_tasks =
            reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(vkGetDeviceProcAddr(device->device, "vkCmdDrawMeshTasksEXT"));
        device->cmd_draw_mesh_tasks_indirect = reinterpret_cast<PFN_vkCmdDrawMeshTasksIndirectEXT>(
            vkGetDeviceProcAddr(device->device, "vkCmdDrawMeshTasksIndirectEXT"));
        device->cmd_draw_mesh_tasks_indirect_count = reinterpret_cast<PFN_vkCmdDrawMeshTasksIndirectCountEXT>(
            vkGetDeviceProcAddr(device->device, "vkCmdDrawMeshTasksIndirectCountEXT"));
    }

    device->resource_slots.initialize(resource_capacity);
    device->sampler_slots.initialize(sampler_capacity);
    if (!create_descriptor_heap(device, resource_capacity, sampler_capacity))
    {
        const Error error = device->error;
        destroy_device(device);
        return {.error = error};
    }

    if (!select_texture_memory_type(device))
    {
        destroy_device(device);
        return {.error = Error::unsupported};
    }

    // Timestamps need the queue family to expose valid bits; a transfer-only family often does not.
    {
        VkQueueFamilyProperties families[max_queue_families]{};
        uint32 family_count = max_queue_families;
        vkGetPhysicalDeviceQueueFamilyProperties(selected, &family_count, families);
        device->caps.timestamps = device->desc.timestamp_query_count > 0 &&
                                  device->queue_family < family_count &&
                                  families[device->queue_family].timestampValidBits > 0;
    }

    const VkSemaphoreTypeCreateInfo timeline_type{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    const VkSemaphoreCreateInfo timeline_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &timeline_type};
    if (!check(device, vkCreateSemaphore(device->device, &timeline_info, nullptr, &device->frame_timeline)))
    {
        const Error error = device->error;
        destroy_device(device);
        return {.error = error};
    }

    if (windowed)
    {
        VkSurfaceFormatKHR formats[64]{};
        uint32 format_count = 64;
        vkGetPhysicalDeviceSurfaceFormatsKHR(selected, device->surface, &format_count, formats);
        const VkFormat wanted = to_vk(desc.swapchain_format);
        device->swapchain_format = VK_FORMAT_UNDEFINED;
        device->caps.swapchain_format = Format::undefined;
        for (uint32 index = 0; index < format_count; ++index)
        {
            if (formats[index].format == wanted && formats[index].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                device->swapchain_format = wanted;
                device->caps.swapchain_format = desc.swapchain_format;
                break;
            }
        }
        // Fall back to the first surface format wvk can name. Accepting one it cannot name would
        // leave the application unable to build a matching colour target, so it is skipped rather
        // than reported as undefined and failed at PSO creation.
        for (uint32 index = 0; index < format_count && device->swapchain_format == VK_FORMAT_UNDEFINED; ++index)
        {
            const Format mapped = format_from_vk(formats[index].format);
            if (mapped == Format::undefined) continue;
            device->swapchain_format = formats[index].format;
            device->caps.swapchain_format = mapped;
        }
        if (device->swapchain_format == VK_FORMAT_UNDEFINED)
        {
            destroy_device(device);
            return {.error = Error::unsupported};
        }

        const VkCommandPoolCreateInfo pool_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = device->queue_family,
        };
        if (!check(device, vkCreateCommandPool(device->device, &pool_info, nullptr, &device->frame_pool)) ||
            !create_swapchain(device))
        {
            const Error error = device->error;
            destroy_device(device);
            return {.error = error};
        }

        VkCommandBuffer scratch[max_swapchain_images * 2]{};
        const VkCommandBufferAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = device->frame_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = max_swapchain_images * 2,
        };
        if (!check(device, vkAllocateCommandBuffers(device->device, &allocate, scratch)))
        {
            const Error error = device->error;
            destroy_device(device);
            return {.error = error};
        }
        const VkSemaphoreCreateInfo binary_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (uint32 index = 0; index < max_swapchain_images; ++index)
        {
            device->pre_present[index] = scratch[index * 2 + 0];
            device->post_present[index] = scratch[index * 2 + 1];
            if (!check(device, vkCreateSemaphore(device->device, &binary_info, nullptr, &device->present_semaphores[index])) ||
                !check(device, vkCreateSemaphore(device->device, &binary_info, nullptr, &device->acquire_semaphores[index])))
            {
                const Error error = device->error;
                destroy_device(device);
                return {.error = error};
            }
        }
    }

    return {.device = device};
}

void destroy_device(Device* device) noexcept
{
    if (!device) return;
    if (device->device)
    {
        vkDeviceWaitIdle(device->device);

        for (CommandBuffer* commands : device->command_buffers)
        {
            if (!commands) continue;
            if (commands->query_pool) vkDestroyQueryPool(device->device, commands->query_pool, nullptr);
            vkDestroyCommandPool(device->device, commands->pool, nullptr);
            destroy_object(commands);
        }

        destroy_swapchain_objects(device);
        if (device->swapchain) vkDestroySwapchainKHR(device->device, device->swapchain, nullptr);
        for (uint32 index = 0; index < max_swapchain_images; ++index)
        {
            if (device->present_semaphores[index]) vkDestroySemaphore(device->device, device->present_semaphores[index], nullptr);
            if (device->acquire_semaphores[index]) vkDestroySemaphore(device->device, device->acquire_semaphores[index], nullptr);
        }
        if (device->frame_pool) vkDestroyCommandPool(device->device, device->frame_pool, nullptr);
        if (device->frame_timeline) vkDestroySemaphore(device->device, device->frame_timeline, nullptr);
        if (device->pipeline_layout) vkDestroyPipelineLayout(device->device, device->pipeline_layout, nullptr);
        if (device->descriptor_pool) vkDestroyDescriptorPool(device->device, device->descriptor_pool, nullptr);
        if (device->set_layout) vkDestroyDescriptorSetLayout(device->device, device->set_layout, nullptr);
        vkDestroyDevice(device->device, nullptr);
    }
    if (device->surface) vkDestroySurfaceKHR(device->instance, device->surface, nullptr);
    if (device->messenger)
    {
        if (auto destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(device->instance, "vkDestroyDebugUtilsMessengerEXT")))
        {
            destroy_messenger(device->instance, device->messenger, nullptr);
        }
    }
    if (device->instance) vkDestroyInstance(device->instance, nullptr);
    destroy_object(device);
}

const DeviceCaps& get_device_caps(const Device* device) noexcept
{
    static const DeviceCaps empty{};
    return device ? device->caps : empty;
}

Error get_device_error(const Device* device) noexcept
{
    return device ? device->error : Error::driver_error;
}

bool supports_format(const Device* device, Format format, TextureUsage usage) noexcept
{
    VkFormatProperties3 properties3{.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
    VkFormatProperties2 properties{.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &properties3};
    vkGetPhysicalDeviceFormatProperties2(device->physical_device, to_vk(format), &properties);
    const VkFormatFeatureFlags2 features = properties3.optimalTilingFeatures;
    if (has_flag(usage, TextureUsage::sampled) && (features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) == 0) return false;

    constexpr VkFormatFeatureFlags2 storage_bits = VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
                                                   VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
                                                   VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;

    if (has_flag(usage, TextureUsage::storage) && (features & storage_bits) != storage_bits) return false;
    if (has_flag(usage, TextureUsage::color_attachment) && (features & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT) == 0)
        return false;
    if (has_flag(usage, TextureUsage::depth_stencil_attachment) &&
        (features & VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT) == 0)
        return false;
    return true;
}

void wait_idle(Device* device) noexcept
{
    if (alive(device)) vkDeviceWaitIdle(device->device);
}

TimelineSemaphore* create_timeline_semaphore(Device* device, uint64 initial_value) noexcept
{
    if (!alive(device)) return nullptr;
    const VkSemaphoreTypeCreateInfo type_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = initial_value,
    };
    const VkSemaphoreCreateInfo info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type_info};
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (!check(device, vkCreateSemaphore(device->device, &info, nullptr, &semaphore))) return nullptr;

    TimelineSemaphore* result = create_object<TimelineSemaphore>();
    if (!result)
    {
        vkDestroySemaphore(device->device, semaphore, nullptr);
        fail(device, Error::out_of_memory);
        return nullptr;
    }
    result->device = device;
    result->semaphore = semaphore;
    return result;
}

void destroy_timeline_semaphore(TimelineSemaphore* semaphore) noexcept
{
    if (!semaphore) return;
    vkDestroySemaphore(semaphore->device->device, semaphore->semaphore, nullptr);
    destroy_object(semaphore);
}

uint64 completed_value(const TimelineSemaphore* semaphore) noexcept
{
    if (!semaphore) return 0;
    uint64 value = 0;
    vkGetSemaphoreCounterValue(semaphore->device->device, semaphore->semaphore, &value);
    return value;
}

void wait_timeline(TimelinePoint point) noexcept
{
    if (!point.semaphore) return;
    const VkSemaphoreWaitInfo info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &point.semaphore->semaphore,
        .pValues = &point.value,
    };
    vkWaitSemaphores(point.semaphore->device->device, &info, UINT64_MAX);
}

// ---------------------------------------------------------------------------------------------
// 6. Heaps, buffers, textures
// ---------------------------------------------------------------------------------------------

Heap* create_heap(Device* device, uint64 byte_count, MemoryType memory) noexcept
{
    if (!alive(device) || byte_count == 0) return nullptr;

    const uint32 memory_type = select_memory_type(device, ~0u, memory);
    if (memory_type == UINT32_MAX)
    {
        fail(device, Error::unsupported);
        return nullptr;
    }

    VkMemoryAllocateFlagsInfo flags{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                    .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    const VkMemoryAllocateInfo allocate{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags,
        .allocationSize = byte_count,
        .memoryTypeIndex = memory_type,
    };
    VkDeviceMemory allocation = VK_NULL_HANDLE;
    if (!check(device, vkAllocateMemory(device->device, &allocate, nullptr, &allocation))) return nullptr;

    Heap* heap = create_object<Heap>();
    if (!heap)
    {
        vkFreeMemory(device->device, allocation, nullptr);
        fail(device, Error::out_of_memory);
        return nullptr;
    }
    heap->device = device;
    heap->memory = allocation;
    heap->size = byte_count;
    heap->type = memory;

    // Mapped once here and never again. Mapping per placed buffer is what makes two resources in the
    // same allocation collide, since VkDeviceMemory may only be mapped a single time.
    if (memory != MemoryType::gpu_only)
    {
        void* pointer = nullptr;
        if (!check(device, vkMapMemory(device->device, allocation, 0, VK_WHOLE_SIZE, 0, &pointer)))
        {
            vkFreeMemory(device->device, allocation, nullptr);
            destroy_object(heap);
            return nullptr;
        }
        heap->mapped = static_cast<byte*>(pointer);
    }
    return heap;
}

void destroy_heap(Heap* heap) noexcept
{
    if (!heap) return;
    if (heap->mapped) vkUnmapMemory(heap->device->device, heap->memory);
    vkFreeMemory(heap->device->device, heap->memory, nullptr);
    destroy_object(heap);
}

byte* mapped_pointer(Heap* heap) noexcept
{
    return heap ? heap->mapped : nullptr;
}

TextureHeap* create_texture_heap(Device* device, uint64 byte_count) noexcept
{
    if (!alive(device) || byte_count == 0) return nullptr;
    // No memory type argument: the device already resolved the single type every image can bind to.
    // No DEVICE_ADDRESS flag either, since images have no GPU address.
    const VkMemoryAllocateInfo allocate{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = byte_count,
        .memoryTypeIndex = device->texture_memory_type,
    };
    VkDeviceMemory allocation = VK_NULL_HANDLE;
    if (!check(device, vkAllocateMemory(device->device, &allocate, nullptr, &allocation))) return nullptr;

    TextureHeap* heap = create_object<TextureHeap>();
    if (!heap)
    {
        vkFreeMemory(device->device, allocation, nullptr);
        fail(device, Error::out_of_memory);
        return nullptr;
    }
    heap->device = device;
    heap->memory = allocation;
    heap->size = byte_count;
    return heap;
}

void destroy_texture_heap(TextureHeap* heap) noexcept
{
    if (!heap) return;
    vkFreeMemory(heap->device->device, heap->memory, nullptr);
    destroy_object(heap);
}

SizeAlign get_buffer_size_align(Device* device, const BufferDesc& desc) noexcept
{
    if (!alive(device)) return {};
    const VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = desc.byte_count,
        .usage = usage_of(desc.usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer probe = VK_NULL_HANDLE;
    if (!check(device, vkCreateBuffer(device->device, &info, nullptr, &probe))) return {};
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device->device, probe, &requirements);
    vkDestroyBuffer(device->device, probe, nullptr);
    return {.size = requirements.size, .align = requirements.alignment};
}

SizeAlign get_texture_size_align(Device* device, const TextureDesc& desc) noexcept
{
    if (!alive(device)) return {};
    const VkImageCreateInfo info = image_info_of(desc);
    VkImage probe = VK_NULL_HANDLE;
    if (!check(device, vkCreateImage(device->device, &info, nullptr, &probe))) return {};
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device->device, probe, &requirements);
    vkDestroyImage(device->device, probe, nullptr);
    return {.size = requirements.size, .align = requirements.alignment};
}

Buffer* create_buffer(Device* device, const BufferDesc& desc) noexcept
{
    if (!alive(device) || desc.byte_count == 0) return nullptr;
    if (desc.heap && desc.heap->type != desc.memory)
    {
        fail(device, Error::unsupported);
        return nullptr;
    }

    const VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = desc.byte_count,
        .usage = usage_of(desc.usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer handle = VK_NULL_HANDLE;
    if (!check(device, vkCreateBuffer(device->device, &info, nullptr, &handle))) return nullptr;

    Buffer* buffer = create_object<Buffer>();
    if (!buffer)
    {
        vkDestroyBuffer(device->device, handle, nullptr);
        fail(device, Error::out_of_memory);
        return nullptr;
    }
    buffer->device = device;
    buffer->buffer = handle;
    buffer->size = desc.byte_count;
    buffer->memory_type = desc.memory;

    if (desc.heap)
    {
        if (!check(device, vkBindBufferMemory(device->device, handle, desc.heap->memory, desc.heap_offset)))
        {
            vkDestroyBuffer(device->device, handle, nullptr);
            destroy_object(buffer);
            return nullptr;
        }
        // The heap owns the single mapping; a placed buffer just points into it.
        if (desc.heap->mapped) buffer->mapped = desc.heap->mapped + desc.heap_offset;
        return buffer;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device->device, handle, &requirements);
    const uint32 memory_type = select_memory_type(device, requirements.memoryTypeBits, desc.memory);
    if (memory_type == UINT32_MAX)
    {
        fail(device, Error::unsupported);
        vkDestroyBuffer(device->device, handle, nullptr);
        destroy_object(buffer);
        return nullptr;
    }

    VkMemoryAllocateFlagsInfo flags{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                    .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    const VkMemoryAllocateInfo allocate{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type,
    };
    if (!check(device, vkAllocateMemory(device->device, &allocate, nullptr, &buffer->owned_memory)) ||
        !check(device, vkBindBufferMemory(device->device, handle, buffer->owned_memory, 0)))
    {
        if (buffer->owned_memory) vkFreeMemory(device->device, buffer->owned_memory, nullptr);
        vkDestroyBuffer(device->device, handle, nullptr);
        destroy_object(buffer);
        return nullptr;
    }

    if (desc.memory != MemoryType::gpu_only)
    {
        void* pointer = nullptr;
        if (!check(device, vkMapMemory(device->device, buffer->owned_memory, 0, VK_WHOLE_SIZE, 0, &pointer)))
        {
            destroy_buffer(buffer);
            return nullptr;
        }
        buffer->mapped = static_cast<byte*>(pointer);
    }
    return buffer;
}

void destroy_buffer(Buffer* buffer) noexcept
{
    if (!buffer) return;
    Device* device = buffer->device;
    if (buffer->owned_memory)
    {
        if (buffer->mapped) vkUnmapMemory(device->device, buffer->owned_memory);
        vkFreeMemory(device->device, buffer->owned_memory, nullptr);
    }
    vkDestroyBuffer(device->device, buffer->buffer, nullptr);
    destroy_object(buffer);
}

byte* mapped_pointer(Buffer* buffer) noexcept
{
    return buffer ? buffer->mapped : nullptr;
}

uint64 get_buffer_size(const Buffer* buffer) noexcept
{
    return buffer ? buffer->size : 0;
}

namespace
{

void push_pending_texture(Device* device, Texture* texture)
{
    device->pending_textures.push_back(texture);
}

void drop_pending_texture(Device* device, Texture* texture) noexcept
{
    for (size_t index = 0; index < device->pending_textures.size(); ++index)
    {
        if (device->pending_textures[index] != texture) continue;
        device->pending_textures[index] = device->pending_textures.back();
        device->pending_textures.pop_back();
        return;
    }
}

} // namespace

Texture* create_texture(Device* device, const TextureDesc& desc) noexcept
{
    if (!alive(device)) return nullptr;

    const VkImageCreateInfo info = image_info_of(desc);
    VkImage image = VK_NULL_HANDLE;
    if (!check(device, vkCreateImage(device->device, &info, nullptr, &image))) return nullptr;

    Texture* texture = create_object<Texture>();
    if (!texture)
    {
        vkDestroyImage(device->device, image, nullptr);
        fail(device, Error::out_of_memory);
        return nullptr;
    }
    texture->device = device;
    texture->image = image;
    texture->desc = desc;

    if (desc.heap)
    {
        // The probe covers every image shape wvk exposes, but an exotic usage combination could still
        // fall outside it; refusing here beats a driver-dependent bind failure later.
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device->device, image, &requirements);
        if ((requirements.memoryTypeBits & (1u << device->texture_memory_type)) == 0)
        {
            fail(device, Error::unsupported);
            vkDestroyImage(device->device, image, nullptr);
            destroy_object(texture);
            return nullptr;
        }
        if (!check(device, vkBindImageMemory(device->device, image, desc.heap->memory, desc.heap_offset)))
        {
            vkDestroyImage(device->device, image, nullptr);
            destroy_object(texture);
            return nullptr;
        }
    }
    else
    {
        // A dedicated allocation is free to use the image's own, possibly wider, memoryTypeBits.
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device->device, image, &requirements);
        const uint32 memory_type = select_memory_type(device, requirements.memoryTypeBits, MemoryType::gpu_only);
        if (memory_type == UINT32_MAX)
        {
            fail(device, Error::unsupported);
            vkDestroyImage(device->device, image, nullptr);
            destroy_object(texture);
            return nullptr;
        }
        const VkMemoryAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = memory_type,
        };
        if (!check(device, vkAllocateMemory(device->device, &allocate, nullptr, &texture->owned_memory)) ||
            !check(device, vkBindImageMemory(device->device, image, texture->owned_memory, 0)))
        {
            if (texture->owned_memory) vkFreeMemory(device->device, texture->owned_memory, nullptr);
            vkDestroyImage(device->device, image, nullptr);
            destroy_object(texture);
            return nullptr;
        }
    }

    // Queued for the one-time UNDEFINED -> GENERAL transition. After that the image never changes
    // layout again, which is precisely what allows barrier() to name no resource.
    push_pending_texture(device, texture);
    return texture;
}

void destroy_texture(Texture* texture) noexcept
{
    if (!texture) return;
    Device* device = texture->device;
    drop_pending_texture(device, texture);
    if (texture->owns_image && texture->image) vkDestroyImage(device->device, texture->image, nullptr);
    if (texture->owned_memory) vkFreeMemory(device->device, texture->owned_memory, nullptr);
    destroy_object(texture);
}

TextureDesc get_texture_desc(const Texture* texture) noexcept
{
    return texture ? texture->desc : TextureDesc{};
}

// ---------------------------------------------------------------------------------------------
// 7. Views, samplers, render views
// ---------------------------------------------------------------------------------------------

BufferView* create_buffer_view(Device* device, const BufferViewDesc& desc) noexcept
{
    if (!alive(device) || !desc.buffer) return nullptr;

    const bool texel = desc.type == BufferViewType::uniform_texel || desc.type == BufferViewType::storage_texel;
    if (texel && desc.texel_format == Format::undefined)
    {
        fail(device, Error::unsupported);
        return nullptr;
    }

    BufferView* view = create_object<BufferView>();
    if (!view)
    {
        fail(device, Error::out_of_memory);
        return nullptr;
    }
    view->device = device;

    const uint64 range = resolved_range(desc.buffer, desc.offset, desc.size);
    if (texel)
    {
        const VkBufferViewCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
            .buffer = desc.buffer->buffer,
            .format = to_vk(desc.texel_format),
            .offset = desc.offset,
            .range = range,
        };
        if (!check(device, vkCreateBufferView(device->device, &info, nullptr, &view->texel_view)))
        {
            destroy_object(view);
            return nullptr;
        }
        view->slot = write_resource_descriptor(
            device,
            desc.type == BufferViewType::uniform_texel ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
                                                       : VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
            nullptr, nullptr, &view->texel_view);
    }
    else
    {
        // StructuredBuffer, RWStructuredBuffer, ByteAddressBuffer and RWByteAddressBuffer all land on
        // the same SPIR-V storage buffer; read-only-ness is a shader-side decoration, not a
        // descriptor type, so one branch covers all four.
        const VkDescriptorBufferInfo info{.buffer = desc.buffer->buffer, .offset = desc.offset, .range = range};
        view->slot = write_resource_descriptor(device, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &info, nullptr);
    }

    if (view->slot == invalid_handle)
    {
        if (view->texel_view) vkDestroyBufferView(device->device, view->texel_view, nullptr);
        destroy_object(view);
        return nullptr;
    }
    return view;
}

void destroy_buffer_view(BufferView* view) noexcept
{
    if (!view) return;
    view->device->resource_slots.release(view->slot);
    if (view->texel_view) vkDestroyBufferView(view->device->device, view->texel_view, nullptr);
    destroy_object(view);
}

ResourceHandle get_handle(const BufferView* view) noexcept
{
    return view ? view->slot : invalid_handle;
}

namespace
{

[[nodiscard]] VkImageViewType view_type_of(TextureType type, bool storage) noexcept
{
    switch (type)
    {
    case TextureType::one_d:       return VK_IMAGE_VIEW_TYPE_1D;
    case TextureType::two_d:       return VK_IMAGE_VIEW_TYPE_2D;
    case TextureType::two_d_array: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case TextureType::three_d:     return VK_IMAGE_VIEW_TYPE_3D;
    // There is no storage cube view in Vulkan; the six faces become a 2D array instead.
    case TextureType::cube:        return storage ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    }
    return VK_IMAGE_VIEW_TYPE_2D;
}

} // namespace

TextureView* create_texture_view(Device* device, const TextureViewDesc& desc) noexcept
{
    if (!alive(device) || !desc.texture) return nullptr;

    const Format format = desc.format != Format::undefined ? desc.format : desc.texture->desc.format;
    const bool storage = desc.type == TextureViewType::storage;

    const VkImageViewCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = desc.texture->image,
        .viewType = view_type_of(desc.texture->desc.type, storage),
        .format = to_vk(format),
        .subresourceRange =
            {
                .aspectMask = aspect_of(format, desc.aspect),
                .baseMipLevel = desc.base_mip,
                .levelCount = desc.mip_count == 0 ? VK_REMAINING_MIP_LEVELS : desc.mip_count,
                .baseArrayLayer = desc.base_layer,
                .layerCount = desc.layer_count == 0 ? VK_REMAINING_ARRAY_LAYERS : desc.layer_count,
            },
    };

    TextureView* view = create_object<TextureView>();
    if (!view)
    {
        fail(device, Error::out_of_memory);
        return nullptr;
    }
    view->device = device;
    if (!check(device, vkCreateImageView(device->device, &info, nullptr, &view->image_view)))
    {
        destroy_object(view);
        return nullptr;
    }

    // Every texture lives in GENERAL, so the descriptor's layout is GENERAL too, for sampled reads
    // as much as for storage writes. That uniformity is the whole point of the layout policy.
    const VkDescriptorImageInfo image_info{.imageView = view->image_view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    view->slot = write_resource_descriptor(
        device, storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &image_info, nullptr,
        nullptr);
    if (view->slot == invalid_handle)
    {
        vkDestroyImageView(device->device, view->image_view, nullptr);
        destroy_object(view);
        return nullptr;
    }
    return view;
}

void destroy_texture_view(TextureView* view) noexcept
{
    if (!view) return;
    view->device->resource_slots.release(view->slot);
    vkDestroyImageView(view->device->device, view->image_view, nullptr);
    destroy_object(view);
}

ResourceHandle get_handle(const TextureView* view) noexcept
{
    return view ? view->slot : invalid_handle;
}

Sampler* create_sampler(Device* device, const SamplerDesc& desc) noexcept
{
    if (!alive(device)) return nullptr;

    const float anisotropy = minimum(maximum(desc.max_anisotropy, 1.0f), device->caps.max_anisotropy);
    const VkSamplerCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = to_vk(desc.mag_filter),
        .minFilter = to_vk(desc.min_filter),
        .mipmapMode = to_mipmap_mode(desc.mip_filter),
        .addressModeU = to_vk(desc.address_u),
        .addressModeV = to_vk(desc.address_v),
        .addressModeW = to_vk(desc.address_w),
        .anisotropyEnable = anisotropy > 1.0f ? VK_TRUE : VK_FALSE,
        .maxAnisotropy = anisotropy,
        .compareEnable = desc.compare_enabled ? VK_TRUE : VK_FALSE,
        .compareOp = to_vk(desc.compare),
        .minLod = desc.min_lod,
        .maxLod = desc.max_lod,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
    };

    Sampler* sampler = create_object<Sampler>();
    if (!sampler)
    {
        fail(device, Error::out_of_memory);
        return nullptr;
    }
    sampler->device = device;
    if (!check(device, vkCreateSampler(device->device, &info, nullptr, &sampler->sampler)))
    {
        destroy_object(sampler);
        return nullptr;
    }

    const uint32 slot = device->sampler_slots.allocate();
    if (slot == invalid_handle)
    {
        fail(device, Error::heap_exhausted);
        vkDestroySampler(device->device, sampler->sampler, nullptr);
        destroy_object(sampler);
        return nullptr;
    }
    const VkDescriptorImageInfo image_info{.sampler = sampler->sampler};
    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = device->descriptor_set,
        .dstBinding = 1,
        .dstArrayElement = slot,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
        .pImageInfo = &image_info,
    };
    vkUpdateDescriptorSets(device->device, 1, &write, 0, nullptr);
    sampler->slot = slot;
    return sampler;
}

void destroy_sampler(Sampler* sampler) noexcept
{
    if (!sampler) return;
    sampler->device->sampler_slots.release(sampler->slot);
    vkDestroySampler(sampler->device->device, sampler->sampler, nullptr);
    destroy_object(sampler);
}

ResourceHandle get_handle(const Sampler* sampler) noexcept
{
    return sampler ? sampler->slot : invalid_handle;
}

RenderView* create_render_view(Texture* texture, const RenderViewDesc& desc) noexcept
{
    if (!texture || !alive(texture->device)) return nullptr;
    Device* device = texture->device;
    const Format format = texture->desc.format;
    const FormatInfo info = get_format_info(format);

    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (info.depth || info.stencil)
    {
        aspect = 0;
        if (info.depth) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (info.stencil) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }

    // A single mip and single layer: one cube face or array slice is a valid 2D attachment.
    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = to_vk(format),
        .subresourceRange = {aspect, desc.mip_level, 1, desc.layer, 1},
    };
    VkImageView image_view = VK_NULL_HANDLE;
    if (!check(device, vkCreateImageView(device->device, &view_info, nullptr, &image_view))) return nullptr;

    RenderView* view = create_object<RenderView>();
    if (!view)
    {
        vkDestroyImageView(device->device, image_view, nullptr);
        fail(device, Error::out_of_memory);
        return nullptr;
    }
    const uint32 shift = desc.mip_level;
    view->device = device;
    view->image_view = image_view;
    view->format = format;
    view->extent = {maximum(texture->desc.extent.x >> shift, 1u), maximum(texture->desc.extent.y >> shift, 1u)};
    view->depth = info.depth;
    view->stencil = info.stencil;
    return view;
}

void destroy_render_view(RenderView* render_view) noexcept
{
    if (!render_view) return;
    vkDestroyImageView(render_view->device->device, render_view->image_view, nullptr);
    destroy_object(render_view);
}

// ---------------------------------------------------------------------------------------------
// 8. Pipelines
// ---------------------------------------------------------------------------------------------

namespace
{

[[nodiscard]] VkShaderModule create_module(Device* device, std::span<const uint32> spirv) noexcept
{
    if (spirv.empty()) return VK_NULL_HANDLE;
    const VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spirv.size_bytes(),
        .pCode = spirv.data(),
    };
    VkShaderModule module = VK_NULL_HANDLE;
    // A null module is the failure signal here; the sticky device error is set by check() either way.
    (void)check(device, vkCreateShaderModule(device->device, &info, nullptr, &module));
    return module;
}

struct RasterCommon
{
    VkPipelineRasterizationStateCreateInfo rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineViewportStateCreateInfo viewport{.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                               .viewportCount = 1,
                                               .scissorCount = 1};
    VkPipelineColorBlendAttachmentState blends[max_color_attachments]{};
    VkPipelineColorBlendStateCreateInfo blend{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkFormat color_formats[max_color_attachments]{};
    VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo depth_stencil{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineDynamicStateCreateInfo dynamic{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
};

// Depth and stencil state is fully dynamic so a PSO never has to be respecialized for it; the same
// list is used by both pipeline flavours.
constexpr VkDynamicState dynamic_states[]{
    VK_DYNAMIC_STATE_VIEWPORT,
    VK_DYNAMIC_STATE_SCISSOR,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
    VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
    VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
    VK_DYNAMIC_STATE_STENCIL_OP,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
    VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
    VK_DYNAMIC_STATE_STENCIL_REFERENCE,
};

void fill_raster_common(Device* device, RasterCommon& out, const RasterizationState& raster,
                        std::span<const ColorTargetDesc> targets, Format depth_format, Format stencil_format) noexcept
{
    out.rasterization.polygonMode =
        raster.fill == FillMode::wireframe && device->caps.fill_mode_non_solid ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    out.rasterization.cullMode = raster.cull == CullMode::none ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    out.rasterization.frontFace =
        raster.cull == CullMode::clockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    out.rasterization.depthClampEnable = raster.depth_clamp && device->caps.depth_clamp ? VK_TRUE : VK_FALSE;
    out.rasterization.depthBiasEnable =
        raster.depth_bias_constant != 0.0f || raster.depth_bias_slope != 0.0f ? VK_TRUE : VK_FALSE;
    out.rasterization.depthBiasConstantFactor = raster.depth_bias_constant;
    out.rasterization.depthBiasClamp = raster.depth_bias_clamp;
    out.rasterization.depthBiasSlopeFactor = raster.depth_bias_slope;
    out.rasterization.lineWidth = 1.0f;

    const uint32 count = minimum<uint32>(static_cast<uint32>(targets.size()), max_color_attachments);
    for (uint32 index = 0; index < count; ++index)
    {
        const ColorTargetDesc& target = targets[index];
        out.color_formats[index] = to_vk(target.format);
        out.blends[index].blendEnable = target.blend.enabled ? VK_TRUE : VK_FALSE;
        out.blends[index].srcColorBlendFactor = to_vk(target.blend.color.source);
        out.blends[index].dstColorBlendFactor = to_vk(target.blend.color.destination);
        out.blends[index].colorBlendOp = to_vk(target.blend.color.operation);
        out.blends[index].srcAlphaBlendFactor = to_vk(target.blend.alpha.source);
        out.blends[index].dstAlphaBlendFactor = to_vk(target.blend.alpha.destination);
        out.blends[index].alphaBlendOp = to_vk(target.blend.alpha.operation);
        out.blends[index].colorWriteMask = target.write_mask & 0xf;
    }
    out.blend.attachmentCount = count;
    out.blend.pAttachments = out.blends;

    out.rendering.colorAttachmentCount = count;
    out.rendering.pColorAttachmentFormats = out.color_formats;
    out.rendering.depthAttachmentFormat = to_vk(depth_format);
    out.rendering.stencilAttachmentFormat = to_vk(stencil_format);

    out.dynamic.dynamicStateCount = static_cast<uint32>(sizeof(dynamic_states) / sizeof(dynamic_states[0]));
    out.dynamic.pDynamicStates = dynamic_states;
}

[[nodiscard]] PSO* finish_pipeline(Device* device, VkGraphicsPipelineCreateInfo& info) noexcept
{
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!check(device, vkCreateGraphicsPipelines(device->device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline)))
        return nullptr;
    PSO* pso = create_object<PSO>();
    if (!pso)
    {
        vkDestroyPipeline(device->device, pipeline, nullptr);
        fail(device, Error::out_of_memory);
        return nullptr;
    }
    pso->device = device;
    pso->pipeline = pipeline;
    pso->bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    return pso;
}

} // namespace

PSO* create_graphics_pso(Device* device, const GraphicsPSODesc& desc) noexcept
{
    if (!alive(device) || desc.vertex_spirv.empty()) return nullptr;

    VkShaderModule vertex = create_module(device, desc.vertex_spirv);
    VkShaderModule fragment = create_module(device, desc.fragment_spirv);
    if (!vertex)
    {
        if (fragment) vkDestroyShaderModule(device->device, fragment, nullptr);
        return nullptr;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    uint32 stage_count = 0;
    stages[stage_count++] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                             .stage = VK_SHADER_STAGE_VERTEX_BIT,
                             .module = vertex,
                             .pName = "main"};
    if (fragment)
    {
        stages[stage_count++] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                 .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                 .module = fragment,
                                 .pName = "main"};
    }

    RasterCommon common;
    fill_raster_common(device, common, desc.rasterization, desc.color_targets, desc.depth_format, desc.stencil_format);

    // Empty on purpose: there are no vertex bindings and no attributes anywhere in wvk. A vertex
    // shader reads its data from a structured buffer indexed by SV_VertexID.
    const VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo input_assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = to_vk(desc.topology),
    };

    VkGraphicsPipelineCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &common.rendering,
        .stageCount = stage_count,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &common.viewport,
        .pRasterizationState = &common.rasterization,
        .pMultisampleState = &common.multisample,
        .pDepthStencilState = &common.depth_stencil,
        .pColorBlendState = &common.blend,
        .pDynamicState = &common.dynamic,
        .layout = device->pipeline_layout,
    };

    PSO* pso = finish_pipeline(device, info);
    vkDestroyShaderModule(device->device, vertex, nullptr);
    if (fragment) vkDestroyShaderModule(device->device, fragment, nullptr);
    return pso;
}

PSO* create_mesh_pso(Device* device, const MeshPSODesc& desc) noexcept
{
    if (!alive(device) || desc.mesh_spirv.empty()) return nullptr;
    if (!device->caps.mesh_shaders) return nullptr; // Optional capability; failure here is expected and quiet.
    // Asking for amplification on a device that reports meshShader but not taskShader is a hard
    // mismatch, not something to silently drop: the mesh stage would receive no payload.
    if (!desc.task_spirv.empty() && !device->caps.task_shaders) return nullptr;

    VkShaderModule task = create_module(device, desc.task_spirv);
    VkShaderModule mesh = create_module(device, desc.mesh_spirv);
    VkShaderModule fragment = create_module(device, desc.fragment_spirv);
    if (!mesh || (!desc.task_spirv.empty() && !task))
    {
        if (task) vkDestroyShaderModule(device->device, task, nullptr);
        if (mesh) vkDestroyShaderModule(device->device, mesh, nullptr);
        if (fragment) vkDestroyShaderModule(device->device, fragment, nullptr);
        return nullptr;
    }

    // Task first, then mesh, then fragment: the order the stages actually run in.
    VkPipelineShaderStageCreateInfo stages[3]{};
    uint32 stage_count = 0;
    if (task)
    {
        stages[stage_count++] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                 .stage = VK_SHADER_STAGE_TASK_BIT_EXT,
                                 .module = task,
                                 .pName = "main"};
    }
    stages[stage_count++] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                             .stage = VK_SHADER_STAGE_MESH_BIT_EXT,
                             .module = mesh,
                             .pName = "main"};
    if (fragment)
    {
        stages[stage_count++] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                 .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                 .module = fragment,
                                 .pName = "main"};
    }

    RasterCommon common;
    fill_raster_common(device, common, desc.rasterization, desc.color_targets, desc.depth_format, desc.stencil_format);

    // VK_EXT_mesh_shader forbids supplying vertex input or input assembly state alongside a mesh
    // stage; topology comes entirely from the shader.
    VkGraphicsPipelineCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &common.rendering,
        .stageCount = stage_count,
        .pStages = stages,
        .pVertexInputState = nullptr,
        .pInputAssemblyState = nullptr,
        .pViewportState = &common.viewport,
        .pRasterizationState = &common.rasterization,
        .pMultisampleState = &common.multisample,
        .pDepthStencilState = &common.depth_stencil,
        .pColorBlendState = &common.blend,
        .pDynamicState = &common.dynamic,
        .layout = device->pipeline_layout,
    };

    PSO* pso = finish_pipeline(device, info);
    if (task) vkDestroyShaderModule(device->device, task, nullptr);
    vkDestroyShaderModule(device->device, mesh, nullptr);
    if (fragment) vkDestroyShaderModule(device->device, fragment, nullptr);
    return pso;
}

PSO* create_compute_pso(Device* device, std::span<const uint32> compute_spirv) noexcept
{
    if (!alive(device) || compute_spirv.empty()) return nullptr;
    VkShaderModule module = create_module(device, compute_spirv);
    if (!module) return nullptr;

    const VkComputePipelineCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module,
                  .pName = "main"},
        .layout = device->pipeline_layout,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    const bool ok = check(device, vkCreateComputePipelines(device->device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline));
    vkDestroyShaderModule(device->device, module, nullptr);
    if (!ok) return nullptr;

    PSO* pso = create_object<PSO>();
    if (!pso)
    {
        vkDestroyPipeline(device->device, pipeline, nullptr);
        fail(device, Error::out_of_memory);
        return nullptr;
    }
    pso->device = device;
    pso->pipeline = pipeline;
    pso->bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
    return pso;
}

void destroy_pso(PSO* pso) noexcept
{
    if (!pso) return;
    vkDestroyPipeline(pso->device->device, pso->pipeline, nullptr);
    destroy_object(pso);
}

// ---------------------------------------------------------------------------------------------
// 9. Command recording
// ---------------------------------------------------------------------------------------------

namespace
{

[[nodiscard]] CommandBuffer* acquire_command_buffer(Device* device) noexcept
{
    uint64 completed = 0;
    vkGetSemaphoreCounterValue(device->device, device->frame_timeline, &completed);

    for (CommandBuffer* candidate : device->command_buffers)
    {
        if (candidate->recording || candidate->retire_value > completed) continue;
        vkResetCommandPool(device->device, candidate->pool, 0);
        return candidate;
    }

    CommandBuffer* commands = create_object<CommandBuffer>();
    if (!commands)
    {
        fail(device, Error::out_of_memory);
        return nullptr;
    }
    commands->device = device;

    const VkCommandPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                            .queueFamilyIndex = device->queue_family};
    if (!check(device, vkCreateCommandPool(device->device, &pool_info, nullptr, &commands->pool)))
    {
        destroy_object(commands);
        return nullptr;
    }
    const VkCommandBufferAllocateInfo allocate{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commands->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (!check(device, vkAllocateCommandBuffers(device->device, &allocate, &commands->commands)))
    {
        vkDestroyCommandPool(device->device, commands->pool, nullptr);
        destroy_object(commands);
        return nullptr;
    }
    // One query pool per command buffer, so slot indices need no cross-buffer bookkeeping.
    if (device->caps.timestamps)
    {
        const uint32 count = device->desc.timestamp_query_count;
        const VkQueryPoolCreateInfo query_info{
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = count,
        };
        commands->timestamp_targets.resize(count);
        if (!check(device, vkCreateQueryPool(device->device, &query_info, nullptr, &commands->query_pool)))
        {
            vkFreeCommandBuffers(device->device, commands->pool, 1, &commands->commands);
            vkDestroyCommandPool(device->device, commands->pool, nullptr);
            destroy_object(commands);
            fail(device, Error::out_of_memory);
            return nullptr;
        }
    }

    device->command_buffers.push_back(commands);
    return commands;
}

void push_root(CommandBuffer* commands, std::span<const std::byte> root) noexcept
{
    if (root.empty()) return;
    const uint32 size = minimum<uint32>(static_cast<uint32>(root.size()), commands->device->caps.max_push_constant_bytes);
    vkCmdPushConstants(commands->commands, commands->device->pipeline_layout, VK_SHADER_STAGE_ALL, 0, size,
                       root.data());
}

void bind_index_buffer(CommandBuffer* commands, BufferRange indices, IndexType type) noexcept
{
    if (!indices.buffer) return;
    vkCmdBindIndexBuffer(commands->commands, indices.buffer->buffer, indices.offset,
                         type == IndexType::uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

[[nodiscard]] VkBufferImageCopy2 image_copy_of(const Texture* texture, uint64 buffer_offset,
                                               const TextureCopyDesc& copy) noexcept
{
    const FormatInfo info = get_format_info(texture->desc.format);
    const uint32 mip = copy.mip_level;
    const uint32x3 extent{
        copy.extent.x != 0 ? copy.extent.x : maximum(texture->desc.extent.x >> mip, 1u),
        copy.extent.y != 0 ? copy.extent.y : maximum(texture->desc.extent.y >> mip, 1u),
        copy.extent.z != 0 ? copy.extent.z
                           : (texture->desc.type == TextureType::three_d ? maximum(texture->desc.extent.z >> mip, 1u) : 1u),
    };

    // Vulkan counts bufferRowLength in texels while the caller supplies bytes, and a compressed
    // format's row is measured in blocks. Converting through the block description keeps BC and ASTC
    // correct without a separate path.
    VkBufferImageCopy2 region{.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
    region.bufferOffset = buffer_offset;
    region.bufferRowLength =
        copy.row_pitch_bytes != 0
            ? static_cast<uint32>(copy.row_pitch_bytes / info.bytes_per_block) * info.block_extent.x
            : 0;
    region.bufferImageHeight =
        (copy.slice_pitch_bytes != 0 && copy.row_pitch_bytes != 0)
            ? static_cast<uint32>(copy.slice_pitch_bytes / copy.row_pitch_bytes) * info.block_extent.y
            : 0;
    region.imageSubresource = {
        .aspectMask = aspect_of(texture->desc.format, TextureAspect::automatic),
        .mipLevel = mip,
        .baseArrayLayer = copy.base_layer,
        .layerCount = maximum(copy.layer_count, 1u),
    };
    region.imageOffset = {static_cast<int32>(copy.offset.x), static_cast<int32>(copy.offset.y),
                          static_cast<int32>(copy.offset.z)};
    region.imageExtent = {extent.x, extent.y, extent.z};
    return region;
}

} // namespace

CommandBuffer* begin_commands(Device* device) noexcept
{
    if (!alive(device)) return nullptr;
    CommandBuffer* commands = acquire_command_buffer(device);
    if (!commands) return nullptr;

    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (!check(device, vkBeginCommandBuffer(commands->commands, &begin))) return nullptr;
    commands->recording = true;
    commands->in_render_pass = false;
    commands->timestamp_count = 0;
    if (commands->query_pool != VK_NULL_HANDLE)
    {
        vkCmdResetQueryPool(commands->commands, commands->query_pool, 0, device->desc.timestamp_query_count);
    }

    // Drain the one-time image initializations. Textures created since the last begin move to
    // GENERAL here and stay there for good.
    if (!device->pending_textures.empty())
    {
        constexpr size_t batch = 32;
        VkImageMemoryBarrier2 barriers[batch]{};
        uint32 written = 0;
        for (size_t index = 0; index < device->pending_textures.size(); ++index)
        {
            const Texture* texture = device->pending_textures[index];
            barriers[written++] = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                .srcAccessMask = 0,
                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = texture->image,
                .subresourceRange = {aspect_of(texture->desc.format, TextureAspect::automatic), 0,
                                     VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS},
            };
            if (written == batch || index + 1 == device->pending_textures.size())
            {
                const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                  .imageMemoryBarrierCount = written,
                                                  .pImageMemoryBarriers = barriers};
                vkCmdPipelineBarrier2(commands->commands, &dependency);
                written = 0;
            }
        }
        device->pending_textures.clear();
    }

    // One set, bound once, for both bind points. Nothing rebinds descriptors after this.
    vkCmdBindDescriptorSets(commands->commands, VK_PIPELINE_BIND_POINT_GRAPHICS, device->pipeline_layout, 0, 1,
                            &device->descriptor_set, 0, nullptr);
    vkCmdBindDescriptorSets(commands->commands, VK_PIPELINE_BIND_POINT_COMPUTE, device->pipeline_layout, 0, 1,
                            &device->descriptor_set, 0, nullptr);
    return commands;
}

void barrier(CommandBuffer* commands, Stage before, Access before_access, Stage after, Access after_access) noexcept
{
    if (!commands || !alive(commands->device)) return;
    const DeviceCaps& caps = commands->device->caps;
    const VkMemoryBarrier2 memory{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = to_vk(before, caps.mesh_shaders, caps.task_shaders),
        .srcAccessMask = to_vk(before_access),
        .dstStageMask = to_vk(after, caps.mesh_shaders, caps.task_shaders),
        .dstAccessMask = to_vk(after_access),
    };
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .memoryBarrierCount = 1,
                                      .pMemoryBarriers = &memory};
    vkCmdPipelineBarrier2(commands->commands, &dependency);
}

void copy_buffer(CommandBuffer* commands, BufferRange source, BufferRange destination) noexcept
{
    if (!commands || !source.buffer || !destination.buffer) return;
    const uint64 source_size = resolved_range(source.buffer, source.offset, source.size);
    const uint64 destination_size = resolved_range(destination.buffer, destination.offset, destination.size);
    const VkBufferCopy2 region{.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
                               .srcOffset = source.offset,
                               .dstOffset = destination.offset,
                               .size = minimum(source_size, destination_size)};
    const VkCopyBufferInfo2 info{.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
                                 .srcBuffer = source.buffer->buffer,
                                 .dstBuffer = destination.buffer->buffer,
                                 .regionCount = 1,
                                 .pRegions = &region};
    vkCmdCopyBuffer2(commands->commands, &info);
}

void copy_buffer_to_texture(CommandBuffer* commands, BufferRange source, Texture* destination,
                            const TextureCopyDesc& copy) noexcept
{
    if (!commands || !source.buffer || !destination) return;
    const VkBufferImageCopy2 region = image_copy_of(destination, source.offset, copy);
    const VkCopyBufferToImageInfo2 info{.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
                                        .srcBuffer = source.buffer->buffer,
                                        .dstImage = destination->image,
                                        .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .regionCount = 1,
                                        .pRegions = &region};
    vkCmdCopyBufferToImage2(commands->commands, &info);
}

void copy_texture_to_buffer(CommandBuffer* commands, Texture* source, BufferRange destination,
                            const TextureCopyDesc& copy) noexcept
{
    if (!commands || !source || !destination.buffer) return;
    const VkBufferImageCopy2 region = image_copy_of(source, destination.offset, copy);
    const VkCopyImageToBufferInfo2 info{.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
                                        .srcImage = source->image,
                                        .srcImageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .dstBuffer = destination.buffer->buffer,
                                        .regionCount = 1,
                                        .pRegions = &region};
    vkCmdCopyImageToBuffer2(commands->commands, &info);
}

void copy_texture_to_texture(CommandBuffer* commands, Texture* source, Texture* destination,
                             const TextureToTextureCopyDesc& copy) noexcept
{
    if (!commands || !source || !destination) return;

    const uint32 mip = copy.source_mip;
    const uint32x3 extent{
        copy.extent.x != 0 ? copy.extent.x : maximum(source->desc.extent.x >> mip, 1u),
        copy.extent.y != 0 ? copy.extent.y : maximum(source->desc.extent.y >> mip, 1u),
        copy.extent.z != 0 ? copy.extent.z
                           : (source->desc.type == TextureType::three_d ? maximum(source->desc.extent.z >> mip, 1u) : 1u),
    };
    const uint32 layers = maximum(copy.layer_count, 1u);

    const VkImageCopy2 region{
        .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
        .srcSubresource = {aspect_of(source->desc.format, TextureAspect::automatic), copy.source_mip,
                           copy.source_base_layer, layers},
        .srcOffset = {static_cast<int32>(copy.source_offset.x), static_cast<int32>(copy.source_offset.y),
                      static_cast<int32>(copy.source_offset.z)},
        .dstSubresource = {aspect_of(destination->desc.format, TextureAspect::automatic), copy.destination_mip,
                           copy.destination_base_layer, layers},
        .dstOffset = {static_cast<int32>(copy.destination_offset.x), static_cast<int32>(copy.destination_offset.y),
                      static_cast<int32>(copy.destination_offset.z)},
        .extent = {extent.x, extent.y, extent.z},
    };
    // Both images are already in GENERAL, so no transition wraps this copy.
    const VkCopyImageInfo2 info{
        .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
        .srcImage = source->image,
        .srcImageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .dstImage = destination->image,
        .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .regionCount = 1,
        .pRegions = &region,
    };
    vkCmdCopyImage2(commands->commands, &info);
}

// A timestamp is written into this command buffer's own query pool and resolved into the caller's
// readback buffer when the command buffer ends, so nothing ever blocks on vkGetQueryPoolResults.
void write_timestamp(CommandBuffer* commands, BufferRange destination, Stage stage) noexcept
{
    if (!commands || !destination.buffer) return;
    Device* device = commands->device;
    if (!device->caps.timestamps || commands->query_pool == VK_NULL_HANDLE) return;
    if (commands->timestamp_count >= device->desc.timestamp_query_count) return;

    const uint32 slot = commands->timestamp_count++;
    commands->timestamp_targets[slot] = {.buffer = destination.buffer->buffer, .offset = destination.offset};
    vkCmdWriteTimestamp2(commands->commands, to_vk(stage, device->caps.mesh_shaders, device->caps.task_shaders),
                         commands->query_pool, slot);
}

void begin_render_pass(CommandBuffer* commands, const RenderingDesc& desc) noexcept
{
    if (!commands || !alive(commands->device)) return;

    VkRenderingAttachmentInfo colors[max_color_attachments]{};
    const uint32 color_count = minimum<uint32>(static_cast<uint32>(desc.colors.size()), max_color_attachments);
    uint32x2 area{};

    for (uint32 index = 0; index < color_count; ++index)
    {
        const ColorAttachment& attachment = desc.colors[index];
        if (!attachment.render_view) continue;
        colors[index] = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = attachment.render_view->image_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = to_vk(attachment.load),
            .storeOp = to_vk(attachment.store),
        };
        colors[index].clearValue.color.float32[0] = attachment.clear.x;
        colors[index].clearValue.color.float32[1] = attachment.clear.y;
        colors[index].clearValue.color.float32[2] = attachment.clear.z;
        colors[index].clearValue.color.float32[3] = attachment.clear.w;
        area = attachment.render_view->extent;
    }

    VkRenderingAttachmentInfo depth{};
    VkRenderingAttachmentInfo stencil{};
    if (desc.depth.render_view)
    {
        depth = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = desc.depth.render_view->image_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = to_vk(desc.depth.load),
            .storeOp = to_vk(desc.depth.store),
        };
        depth.clearValue.depthStencil.depth = desc.depth.clear;
        area = desc.depth.render_view->extent;
    }
    if (desc.stencil.render_view)
    {
        stencil = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = desc.stencil.render_view->image_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = to_vk(desc.stencil.load),
            .storeOp = to_vk(desc.stencil.store),
        };
        stencil.clearValue.depthStencil.stencil = desc.stencil.clear;
        area = desc.stencil.render_view->extent;
    }

    const VkRenderingInfo info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, {area.x, area.y}},
        .layerCount = 1,
        .colorAttachmentCount = color_count,
        .pColorAttachments = color_count > 0 ? colors : nullptr,
        .pDepthAttachment = desc.depth.render_view ? &depth : nullptr,
        .pStencilAttachment = desc.stencil.render_view ? &stencil : nullptr,
    };
    vkCmdBeginRendering(commands->commands, &info);
    commands->in_render_pass = true;
    commands->render_area = area;

    // Defaults matching the documented contract: full render area, depth and stencil off.
    set_viewport(commands, 0.0f, 0.0f, static_cast<float>(area.x), static_cast<float>(area.y));
    set_scissor(commands, 0, 0, area.x, area.y);
    set_depth_stencil(commands, {});
}

void end_render_pass(CommandBuffer* commands) noexcept
{
    if (!commands || !commands->in_render_pass) return;
    vkCmdEndRendering(commands->commands);
    commands->in_render_pass = false;
}

void set_viewport(CommandBuffer* commands, float x, float y, float width, float height, float min_depth,
                  float max_depth) noexcept
{
    if (!commands) return;
    // Negative height flips Y so that shaders can keep D3D-style top-left origin conventions.
    const VkViewport viewport{
        .x = x, .y = y + height, .width = width, .height = -height, .minDepth = min_depth, .maxDepth = max_depth};
    vkCmdSetViewport(commands->commands, 0, 1, &viewport);
}

void set_scissor(CommandBuffer* commands, int32 x, int32 y, uint32 width, uint32 height) noexcept
{
    if (!commands) return;
    const VkRect2D scissor{{x, y}, {width, height}};
    vkCmdSetScissor(commands->commands, 0, 1, &scissor);
}

void set_depth_stencil(CommandBuffer* commands, const DepthStencilState& state) noexcept
{
    if (!commands) return;
    VkCommandBuffer raw = commands->commands;
    vkCmdSetDepthTestEnable(raw, state.depth_test ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthWriteEnable(raw, state.depth_write ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthCompareOp(raw, to_vk(state.depth_compare));
    vkCmdSetStencilTestEnable(raw, state.stencil_test ? VK_TRUE : VK_FALSE);
    vkCmdSetStencilOp(raw, VK_STENCIL_FACE_FRONT_BIT, to_vk(state.front.fail), to_vk(state.front.pass),
                      to_vk(state.front.depth_fail), to_vk(state.front.compare));
    vkCmdSetStencilOp(raw, VK_STENCIL_FACE_BACK_BIT, to_vk(state.back.fail), to_vk(state.back.pass),
                      to_vk(state.back.depth_fail), to_vk(state.back.compare));
    vkCmdSetStencilCompareMask(raw, VK_STENCIL_FACE_FRONT_AND_BACK, state.stencil_read_mask);
    vkCmdSetStencilWriteMask(raw, VK_STENCIL_FACE_FRONT_AND_BACK, state.stencil_write_mask);
    vkCmdSetStencilReference(raw, VK_STENCIL_FACE_FRONT_BIT, state.front.reference);
    vkCmdSetStencilReference(raw, VK_STENCIL_FACE_BACK_BIT, state.back.reference);
}

void bind_pso(CommandBuffer* commands, const PSO* pso) noexcept
{
    if (!commands || !pso) return;
    vkCmdBindPipeline(commands->commands, pso->bind_point, pso->pipeline);
}

void draw(CommandBuffer* commands, std::span<const std::byte> root, uint32 vertex_count, uint32 instance_count, uint32 first_vertex,
          uint32 first_instance) noexcept
{
    if (!commands) return;
    push_root(commands, root);
    vkCmdDraw(commands->commands, vertex_count, instance_count, first_vertex, first_instance);
}

void draw_indexed(CommandBuffer* commands, std::span<const std::byte> root, BufferRange indices, IndexType type, uint32 index_count,
                  uint32 instance_count, uint32 first_index, int32 vertex_offset, uint32 first_instance) noexcept
{
    if (!commands) return;
    push_root(commands, root);
    bind_index_buffer(commands, indices, type);
    vkCmdDrawIndexed(commands->commands, index_count, instance_count, first_index, vertex_offset, first_instance);
}

void dispatch(CommandBuffer* commands, std::span<const std::byte> root, uint32x3 group_count) noexcept
{
    if (!commands) return;
    push_root(commands, root);
    vkCmdDispatch(commands->commands, group_count.x, group_count.y, group_count.z);
}

void draw_meshlets(CommandBuffer* commands, std::span<const std::byte> root, uint32x3 group_count) noexcept
{
    if (!commands || !commands->device->cmd_draw_mesh_tasks) return;
    push_root(commands, root);
    commands->device->cmd_draw_mesh_tasks(commands->commands, group_count.x, group_count.y, group_count.z);
}

void draw_indirect(CommandBuffer* commands, std::span<const std::byte> root, BufferRange arguments, uint32 draw_count,
                   uint32 stride) noexcept
{
    if (!commands || !arguments.buffer || draw_count == 0) return;
    push_root(commands, root);
    // Without multiDrawIndirect a single command may only cover one draw, so the batch is unrolled
    // rather than refused. The caller sees the same result either way.
    if (draw_count == 1 || commands->device->caps.multi_draw_indirect)
    {
        vkCmdDrawIndirect(commands->commands, arguments.buffer->buffer, arguments.offset, draw_count, stride);
        return;
    }
    for (uint32 index = 0; index < draw_count; ++index)
    {
        vkCmdDrawIndirect(commands->commands, arguments.buffer->buffer,
                          arguments.offset + static_cast<uint64>(index) * stride, 1, stride);
    }
}

void draw_indexed_indirect(CommandBuffer* commands, std::span<const std::byte> root, BufferRange indices, IndexType type,
                           BufferRange arguments, uint32 draw_count, uint32 stride) noexcept
{
    if (!commands || !arguments.buffer || draw_count == 0) return;
    push_root(commands, root);
    bind_index_buffer(commands, indices, type);
    if (draw_count == 1 || commands->device->caps.multi_draw_indirect)
    {
        vkCmdDrawIndexedIndirect(commands->commands, arguments.buffer->buffer, arguments.offset, draw_count, stride);
        return;
    }
    for (uint32 index = 0; index < draw_count; ++index)
    {
        vkCmdDrawIndexedIndirect(commands->commands, arguments.buffer->buffer,
                                 arguments.offset + static_cast<uint64>(index) * stride, 1, stride);
    }
}

void dispatch_indirect(CommandBuffer* commands, std::span<const std::byte> root, BufferRange arguments) noexcept
{
    if (!commands || !arguments.buffer) return;
    push_root(commands, root);
    vkCmdDispatchIndirect(commands->commands, arguments.buffer->buffer, arguments.offset);
}

void draw_meshlets_indirect(CommandBuffer* commands, std::span<const std::byte> root, BufferRange arguments, uint32 draw_count,
                            uint32 stride) noexcept
{
    if (!commands || !arguments.buffer || draw_count == 0) return;
    Device* device = commands->device;
    if (!device->cmd_draw_mesh_tasks_indirect) return;
    push_root(commands, root);
    if (draw_count == 1 || device->caps.multi_draw_indirect)
    {
        device->cmd_draw_mesh_tasks_indirect(commands->commands, arguments.buffer->buffer, arguments.offset, draw_count,
                                             stride);
        return;
    }
    for (uint32 index = 0; index < draw_count; ++index)
    {
        device->cmd_draw_mesh_tasks_indirect(commands->commands, arguments.buffer->buffer,
                                             arguments.offset + static_cast<uint64>(index) * stride, 1, stride);
    }
}

void draw_indirect_count(CommandBuffer* commands, std::span<const std::byte> root, BufferRange arguments, BufferRange count,
                         uint32 max_draw_count, uint32 stride) noexcept
{
    if (!commands || !arguments.buffer || !count.buffer) return;
    Device* device = commands->device;
    if (!device->caps.draw_indirect_count) return;
    push_root(commands, root);
    vkCmdDrawIndirectCount(commands->commands, arguments.buffer->buffer, arguments.offset, count.buffer->buffer,
                           count.offset, minimum(max_draw_count, device->caps.max_draw_indirect_count), stride);
}

void draw_indexed_indirect_count(CommandBuffer* commands, std::span<const std::byte> root, BufferRange indices, IndexType type,
                                 BufferRange arguments, BufferRange count, uint32 max_draw_count,
                                 uint32 stride) noexcept
{
    if (!commands || !arguments.buffer || !count.buffer) return;
    Device* device = commands->device;
    if (!device->caps.draw_indirect_count) return;
    push_root(commands, root);
    bind_index_buffer(commands, indices, type);
    vkCmdDrawIndexedIndirectCount(commands->commands, arguments.buffer->buffer, arguments.offset, count.buffer->buffer,
                                  count.offset, minimum(max_draw_count, device->caps.max_draw_indirect_count), stride);
}

void draw_meshlets_indirect_count(CommandBuffer* commands, std::span<const std::byte> root, BufferRange arguments, BufferRange count,
                                  uint32 max_draw_count, uint32 stride) noexcept
{
    if (!commands || !arguments.buffer || !count.buffer) return;
    Device* device = commands->device;
    if (!device->caps.draw_indirect_count || !device->cmd_draw_mesh_tasks_indirect_count) return;
    push_root(commands, root);
    device->cmd_draw_mesh_tasks_indirect_count(commands->commands, arguments.buffer->buffer, arguments.offset,
                                               count.buffer->buffer, count.offset,
                                               minimum(max_draw_count, device->caps.max_draw_indirect_count), stride);
}

// ---------------------------------------------------------------------------------------------
// 10. Submission and presentation
// ---------------------------------------------------------------------------------------------

namespace
{

constexpr uint32 max_submitted_command_buffers = 64;

// Copies every recorded timestamp into its destination buffer, then publishes the writes to the
// host. The WAIT bit makes the copy itself the synchronization point, so the application never has
// to call vkGetQueryPoolResults and never stalls the CPU on a query.
void resolve_timestamps(CommandBuffer* commands) noexcept
{
    if (commands->timestamp_count == 0) return;
    for (uint32 index = 0; index < commands->timestamp_count; ++index)
    {
        const TimestampTarget& target = commands->timestamp_targets[index];
        vkCmdCopyQueryPoolResults(commands->commands, commands->query_pool, index, 1, target.buffer, target.offset,
                                  sizeof(uint64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    }
    const VkMemoryBarrier2 to_host{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &to_host};
    vkCmdPipelineBarrier2(commands->commands, &dependency);
}

void submit_internal(Device* device, std::span<CommandBuffer* const> commands, TimelinePoint completion,
                     bool present) noexcept
{
    if (!alive(device)) return;

    VkCommandBufferSubmitInfo infos[max_submitted_command_buffers + 2]{};
    uint32 info_count = 0;

    const bool presenting = present && device->frame_acquired && device->swapchain_image_count > 0;
    if (presenting)
    {
        record_present_transitions(device, device->image_index);
        infos[info_count++] = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                               .commandBuffer = device->pre_present[device->image_index]};
    }

    const uint32 user_count = minimum<uint32>(static_cast<uint32>(commands.size()), max_submitted_command_buffers);
    for (uint32 index = 0; index < user_count; ++index)
    {
        CommandBuffer* buffer = commands[index];
        if (!buffer || !buffer->recording) continue;
        if (buffer->in_render_pass) end_render_pass(buffer);
        resolve_timestamps(buffer);
        vkEndCommandBuffer(buffer->commands);
        buffer->recording = false;
        infos[info_count++] = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = buffer->commands};
    }

    if (presenting)
    {
        infos[info_count++] = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                               .commandBuffer = device->post_present[device->image_index]};
    }

    const uint64 frame_value = ++device->frame_counter;
    for (uint32 index = 0; index < user_count; ++index)
    {
        if (commands[index]) commands[index]->retire_value = frame_value;
    }

    VkSemaphoreSubmitInfo waits[1]{};
    uint32 wait_count = 0;
    if (presenting)
    {
        waits[wait_count++] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                               .semaphore = device->acquire_semaphores[device->frame_index],
                               .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    }

    VkSemaphoreSubmitInfo signals[3]{};
    uint32 signal_count = 0;
    // The internal frame timeline always advances: it is what recycles command pools and gates
    // reuse of the binary acquire semaphores, independently of whatever the application tracks.
    signals[signal_count++] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                               .semaphore = device->frame_timeline,
                               .value = frame_value,
                               .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    if (completion.semaphore)
    {
        signals[signal_count++] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                   .semaphore = completion.semaphore->semaphore,
                                   .value = completion.value,
                                   .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    }
    if (presenting)
    {
        // One binary semaphore per swapchain image, not per frame: presentation completion is tied
        // to the image, and sharing one across images is the classic source of validation errors.
        signals[signal_count++] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                   .semaphore = device->present_semaphores[device->image_index],
                                   .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    }

    const VkSubmitInfo2 submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = wait_count,
        .pWaitSemaphoreInfos = waits,
        .commandBufferInfoCount = info_count,
        .pCommandBufferInfos = infos,
        .signalSemaphoreInfoCount = signal_count,
        .pSignalSemaphoreInfos = signals,
    };
    if (!check(device, vkQueueSubmit2(device->queue, 1, &submit_info, VK_NULL_HANDLE))) return;

    if (!presenting) return;

    device->frame_retire_values[device->frame_index] = frame_value;
    const VkPresentInfoKHR present_info{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &device->present_semaphores[device->image_index],
        .swapchainCount = 1,
        .pSwapchains = &device->swapchain,
        .pImageIndices = &device->image_index,
    };
    const VkResult result = vkQueuePresentKHR(device->queue, &present_info);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        device->swapchain_dirty = true;
    }
    else if (result != VK_SUCCESS)
    {
        fail(device, error_from_vk(result));
    }
    device->frame_acquired = false;
    device->frame_index = (device->frame_index + 1) % maximum(device->swapchain_image_count, 1u);
}

} // namespace

uint32x2 get_drawable_extent(Device* device) noexcept
{
    if (!alive(device) || !device->surface) return {};
    VkSurfaceCapabilitiesKHR capabilities{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->physical_device, device->surface, &capabilities) != VK_SUCCESS)
        return {};
    // Same 0xFFFFFFFF convention as create_swapchain: fall back to what the application told us.
    if (capabilities.currentExtent.width == 0xffffffffu || capabilities.currentExtent.height == 0xffffffffu)
        return device->drawable_hint;
    return {capabilities.currentExtent.width, capabilities.currentExtent.height};
}

void set_drawable_extent(Device* device, uint32x2 extent) noexcept
{
    if (!alive(device)) return;
    device->drawable_hint = extent;
    if (extent.x != device->swapchain_extent.x || extent.y != device->swapchain_extent.y)
    {
        device->swapchain_dirty = true;
    }
}

SwapchainFrame acquire(Device* device) noexcept
{
    if (!alive(device) || !device->surface) return {};

    if (device->swapchain_dirty || device->swapchain_extent.x == 0)
    {
        vkDeviceWaitIdle(device->device);
        device->swapchain_dirty = false;
        if (!create_swapchain(device) || device->swapchain_extent.x == 0) return {};
    }

    // Gate on the frame that last used this slot's acquire semaphore before reusing it. Without
    // swapchain_maintenance1 there is no present fence, so the frame timeline carries the guarantee.
    wait_frame_timeline(device, device->frame_retire_values[device->frame_index]);

    uint32 image = 0;
    const VkResult result = vkAcquireNextImageKHR(device->device, device->swapchain, UINT64_MAX,
                                                  device->acquire_semaphores[device->frame_index], VK_NULL_HANDLE, &image);
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        device->swapchain_dirty = true;
        return {};
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        fail(device, error_from_vk(result));
        return {};
    }

    device->image_index = image;
    device->frame_acquired = true;
    return {.render_view = device->swapchain_views[image], .extent = device->swapchain_extent};
}

void submit(Device* device, std::span<CommandBuffer* const> commands, TimelinePoint completion) noexcept
{
    submit_internal(device, commands, completion, false);
}

void submit_and_present(Device* device, std::span<CommandBuffer* const> commands, TimelinePoint completion) noexcept
{
    submit_internal(device, commands, completion, true);
}

} // namespace wvk
