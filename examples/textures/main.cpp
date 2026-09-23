#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <wvk/wvk.hpp>
#include <vector>
#include <cstring>
#include <cstdio>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

template<typename F>
struct privDefer {
    F f;
    privDefer(F f) : f(f) {}
    ~privDefer() { f(); }
};

template<typename F>
privDefer<F> defer_func(F f) {
    return privDefer<F>(f);
}

#define DEFER_1(x, y) x##y
#define DEFER_2(x, y) DEFER_1(x, y)
#define DEFER_3(x)    DEFER_2(x, __COUNTER__)
#define defer(code)   auto DEFER_3(_defer_) = defer_func([&](){code;})

struct Vertex
{
    float position[2];
    float uv[2];
};

struct PushConstants
{
    wvk::ResourceHandle vertices;
    wvk::ResourceHandle albedo;
    wvk::ResourceHandle albedo_sampler;
};

constexpr Vertex quad_vertices[]{
    {{-0.5f,  0.5f}, {0.0f, 0.0f}},
    {{ 0.5f,  0.5f}, {1.0f, 0.0f}},
    {{-0.5f, -0.5f}, {0.0f, 1.0f}},
    {{ 0.5f, -0.5f}, {1.0f, 1.0f}},
};

struct Image
{
    wvk::uint32 width = 0;
    wvk::uint32 height = 0;
    std::vector<wvk::uint8> rgba;
};

Image load_image(const char* path)
{
    Image image;
    int width = 0, height = 0, channels_in_file = 0;
    if (stbi_uc* pixels = stbi_load(path, &width, &height, &channels_in_file, 4))
    {
        image.width  = static_cast<wvk::uint32>(width);
        image.height = static_cast<wvk::uint32>(height);
        image.rgba.assign(pixels, pixels + size_t(width) * height * 4);
        stbi_image_free(pixels);
        return image;
    }
    std::fprintf(stderr, "stb_image: %s: %s, damier utilise a la place\n", path, stbi_failure_reason());

    image.width = image.height = 256;
    image.rgba.resize(256 * 256 * 4);
    for (wvk::uint32 y = 0; y < 256; ++y)
    for (wvk::uint32 x = 0; x < 256; ++x)
    {
        const bool light = ((x / 32) + (y / 32)) & 1;
        wvk::uint8* p = &image.rgba[(size_t(y) * 256 + x) * 4];
        p[0] = light ? 240 : 40;
        p[1] = light ? 200 : 40;
        p[2] = light ? 80  : 60;
        p[3] = 255;
    }
    return image;
}

std::vector<wvk::uint32> read_spirv(const char* path) {
    size_t byte_count = 0;
    void* bytes = SDL_LoadFile(path, &byte_count);
    if (!bytes) return {};
    if (byte_count == 0 || (byte_count & 3) != 0) { SDL_free(bytes); return {}; }
    std::vector<wvk::uint32> words(byte_count / 4);
    SDL_memcpy(words.data(), bytes, byte_count);
    SDL_free(bytes);
    return words;
}

void report(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "wvk quad", message, nullptr);
}

int main() {
    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_VIDEO)) { report(SDL_GetError()); return 1; }
    defer(SDL_Quit());

    SDL_Window* window = SDL_CreateWindow("wvk quad", 800, 600, SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    if (!window) {
        report(SDL_GetError());
        return 1;
    }
    defer(SDL_DestroyWindow(window));

    constexpr wvk::uint32 frames_in_flight = 2;

    int pixel_width = 0, pixel_height = 0;
    SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height);

    wvk::DeviceDesc device_desc = {};
    device_desc.swapchain_format  = wvk::Format::bgra8_unorm;
    device_desc.drawable_extent   = {static_cast<wvk::uint32>(pixel_width), static_cast<wvk::uint32>(pixel_height)};
    device_desc.frames_in_flight  = frames_in_flight;
    device_desc.enable_validation = true;

    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    #if GAME_LINUX
    wvk::LinuxWindowHandle linux_window_handle = {};
    const bool is_wayland = SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0;
    if (is_wayland) {
        linux_window_handle.display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        linux_window_handle.window = static_cast<wvk::uint64>(reinterpret_cast<wvk::uintptr>(
            SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr)));
    } else {
        linux_window_handle.display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        linux_window_handle.window =
            static_cast<wvk::uint64>(SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    }
    device_desc.display_server = is_wayland ? wvk::DisplayServer::wayland : wvk::DisplayServer::x11;
    device_desc.handle = &linux_window_handle;
    #else
    device_desc.handle = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    #endif

    const wvk::DeviceInit init = wvk::create_device(device_desc);
    if (!init.device) {
        report("create_device failed");
        return 1;
    }
    wvk::Device* device = init.device;
    defer(wvk::destroy_device(device));
    const wvk::DeviceCaps& caps = wvk::get_device_caps(device);

    // --- PSO ---------------------------------------------------------------------------------
    const std::vector<wvk::uint32> vertex_spirv   = read_spirv("quad.vert.spv");
    const std::vector<wvk::uint32> fragment_spirv = read_spirv("quad.frag.spv");
    if (vertex_spirv.empty() || fragment_spirv.empty()) {
        report("Could not read quad.vert.spv / quad.frag.spv");
        return 1;
    }

    const wvk::ColorTargetDesc color_targets[]{{.format = caps.swapchain_format}};
    wvk::PSO* pso = wvk::create_graphics_pso(device, {
        .vertex_spirv   = vertex_spirv,
        .fragment_spirv = fragment_spirv,
        .color_targets  = color_targets,
        .topology       = wvk::Topology::triangle_strip,
        .rasterization  = {.cull = wvk::CullMode::none},
    });
    defer(wvk::destroy_pso(pso));

    // --- Vertex buffer ----------------------------------------------
    wvk::Buffer* vertices = wvk::create_buffer(device, {
        .byte_count = sizeof(quad_vertices),
        .memory = wvk::MemoryType::cpu_to_gpu,
    });
    defer(wvk::destroy_buffer(vertices));
    std::memcpy(wvk::mapped_pointer(vertices), quad_vertices, sizeof(quad_vertices));
    wvk::BufferView* vertex_view = wvk::create_buffer_view(device, {.buffer = vertices});

    // --- Texture -----------------------------------------------------------------------------
    const Image image = load_image("texture.png");

    // gpu_only + transfer_destination.
    // IMPORTANT : create the texture BEFORE of first begin_commands (wvk will do the transition
    // UNDEFINED -> GENERAL).
    wvk::Texture* texture = wvk::create_texture(device, {
        .type   = wvk::TextureType::two_d,
        .extent = {.x = image.width, .y = image.height, .z = 1},
        .format = wvk::Format::rgba8_unorm,
        .usage  = wvk::TextureUsage::sampled | wvk::TextureUsage::transfer_destination,
    });
    defer(destroy_texture(texture));

    // Slot in heap des ressources.
    wvk::TextureView* texture_view = wvk::create_texture_view(device, {.texture = texture});
    defer(destroy_texture_view(texture_view));

    // Slot in heap des samplers.
    wvk::Sampler* sampler = wvk::create_sampler(device, {
        .min_filter = wvk::Filter::linear,
        .mag_filter = wvk::Filter::linear,
        .address_u  = wvk::AddressMode::clamp_to_edge,
        .address_v  = wvk::AddressMode::clamp_to_edge,
    });
    defer(destroy_sampler(sampler));

    wvk::TimelineSemaphore* timeline = wvk::create_timeline_semaphore(device);
    defer(destroy_timeline_semaphore(timeline));

    if (!pso || !vertices || !vertex_view || !texture || !texture_view || !sampler || !timeline) {
        report("resource creation failed");
        return 1;
    }

    // --- Texture upload ----------------------------------------------------------------------
    {
        wvk::Buffer* staging = wvk::create_buffer(device, {
            .byte_count = image.rgba.size(),
            .memory = wvk::MemoryType::cpu_to_gpu,
        });
        defer(wvk::destroy_buffer(staging));
        std::memcpy(wvk::mapped_pointer(staging), image.rgba.data(), image.rgba.size());

        wvk::CommandBuffer* upload = wvk::begin_commands(device);
        wvk::copy_buffer_to_texture(upload, {.buffer = staging}, texture); // mip 0, toute l'image
        // Rend la copie visible aux lectures du fragment shader des soumissions suivantes.
        wvk::barrier(upload, wvk::Stage::transfer, wvk::Access::transfer_write,
                             wvk::Stage::fragment, wvk::Access::shader_read);
        wvk::submit(device, {&upload, 1}, {.semaphore = timeline, .value = 1});

        // We wait at the end of the copie before freeing the staging buffer.
        wvk::wait_timeline({.semaphore = timeline, .value = 1});
    }

    // --- Loop -------------------------------------------------------------------------------
    wvk::uint64 frame = 1;
    bool running = true;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) running = false;
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                wvk::set_drawable_extent(device, {static_cast<wvk::uint32>(event.window.data1),
                                                  static_cast<wvk::uint32>(event.window.data2)});
        }
        if (!running) break;
        if (wvk::get_device_error(device) != wvk::Error::none) { report("device lost"); break; }

        if (frame > frames_in_flight)
            wvk::wait_timeline({.semaphore = timeline, .value = frame - frames_in_flight});

        const wvk::SwapchainFrame swapchain = wvk::acquire(device);
        if (!swapchain.render_view) { SDL_Delay(1); continue; }

        wvk::CommandBuffer* commands = wvk::begin_commands(device);
        if (!commands) break;

        const wvk::ColorAttachment colors[]{{
            .render_view = swapchain.render_view,
            .load  = wvk::LoadOp::clear,
            .store = wvk::StoreOp::store,
            .clear = {.x = 0.02f, .y = 0.02f, .z = 0.04f, .w = 1.0f},
        }};
        wvk::begin_render_pass(commands, {.colors = colors});
        wvk::bind_pso(commands, pso);

        const PushConstants root{
            .vertices       = wvk::get_handle(vertex_view),
            .albedo         = wvk::get_handle(texture_view),
            .albedo_sampler = wvk::get_handle(sampler),
        };
        wvk::draw(commands, wvk::root_of(root), 4);

        wvk::end_render_pass(commands);

        frame++;
        wvk::submit_and_present(device, {&commands, 1}, {.semaphore = timeline, .value = frame});
    }

    wvk::wait_idle(device);
    return 0;
}
