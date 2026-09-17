/* -*- compile-command: "g++ -std=c++20 -DGAME_LINUX=1 -I./include $(pkg-config --cflags sdl3) triangle_new.cpp ./src/wvk.cpp $(pkg-config --libs sdl3) -lvulkan -o triangle_new" -*- */

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <wvk/wvk.hpp>
#include <vector>
#include <cstring>

struct Vertex
{
    float position[3];
    float color[3];
};

struct PushConstants
{
    wvk::ResourceHandle vertices;
};

constexpr Vertex triangle_vertices[]{
    {{ 0.0f, 0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}},
    {{ 0.5f,-0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}},
    {{-0.5f,-0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}},
};

std::vector<wvk::uint32> read_spirv(const char* path) {
    size_t byte_count = 0;
    void* bytes = SDL_LoadFile(path, &byte_count);
    if (!bytes) return {};
    if (byte_count == 0 || (byte_count & 3) != 0)
    {
        SDL_free(bytes);
        return {};
    }
    std::vector<wvk::uint32> words(byte_count / 4);
    SDL_memcpy(words.data(), bytes, byte_count);
    SDL_free(bytes);
    return words;
}

void report(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "wvk triangle", message, nullptr);
}

int main() {
    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        report(SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("wvk triangle", 800, 600, SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    if (!window) {
        report(SDL_GetError());
        SDL_Quit();
        return 1;
    }

    constexpr wvk::uint32 frames_in_flight = 2;

    int pixel_width  = 0;
    int pixel_height = 0;
    SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height);

    wvk::DeviceDesc device_desc = {};
    device_desc.swapchain_format = wvk::Format::bgra8_unorm;
    device_desc.drawable_extent  = {
        static_cast<wvk::uint32>(pixel_width),
        static_cast<wvk::uint32>(pixel_height),
    };
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

    if (!device_desc.handle) {
        report("SDL did not report a native window handle for this platform");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    const wvk::DeviceInit init = wvk::create_device(device_desc);
    if (!init.device) {
        report(init.error == wvk::Error::unsupported
               ? "No GPU meets the wvk requirements: Vulkan 1.3 with VK_EXT_mutable_descriptor_type"
               : "create_device failed");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    wvk::Device* device = init.device;
    const wvk::DeviceCaps& caps = wvk::get_device_caps(device);

    std::printf("device: %s\n", caps.device_name.c_str());
    std::printf("bindless slots: %u resources, %u samplers\n", caps.max_resource_descriptors,
                caps.max_sampler_descriptors);
    std::printf("root size: %u bytes | mesh shaders: %s | multi-draw indirect: %s\n", caps.max_push_constant_bytes,
                caps.mesh_shaders ? "yes" : "no", caps.multi_draw_indirect ? "yes" : "no");

    const std::vector<wvk::uint32> vertex_spirv   = read_spirv("triangle.vert.spv");
    const std::vector<wvk::uint32> fragment_spirv = read_spirv("triangle.frag.spv");
    if (vertex_spirv.empty() || fragment_spirv.empty()) {
        report("Could not read triangle.vert.spv / triangle.frag.spv from the working directory");
        wvk::destroy_device(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    const wvk::ColorTargetDesc color_targets[]{{.format = caps.swapchain_format}};
    wvk::PSO* pso = wvk::create_graphics_pso(device, {
        .vertex_spirv = vertex_spirv,
        .fragment_spirv = fragment_spirv,
        .color_targets = color_targets,
        .topology = wvk::Topology::triangles,
        .rasterization = {.cull = wvk::CullMode::none},
    });

    if (!pso) {
        report("create_graphics_pso failed");
        wvk::destroy_device(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    constexpr wvk::uint64 vertex_bytes = sizeof(triangle_vertices);

    wvk::Buffer* vertices = wvk::create_buffer(device, {
        .byte_count = vertex_bytes,
        .memory = wvk::MemoryType::cpu_to_gpu,
    });

    wvk::TimelineSemaphore* timeline = wvk::create_timeline_semaphore(device);
    if (!vertices || !timeline) {
        report("resource creation failed");
        wvk::destroy_device(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    std::memcpy(wvk::mapped_pointer(vertices), triangle_vertices, vertex_bytes);

    wvk::BufferView* vertex_view = wvk::create_buffer_view(device, {
        .buffer = vertices,
        .type = wvk::BufferViewType::structured,
    });
    if (!vertex_view) {
        report("create_buffer_view failed");
        wvk::destroy_device(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    std::printf("vertex buffer bound at bindless slot %u\n", wvk::get_handle(vertex_view));

    wvk::uint64 frame = 1;
    const wvk::uint64 start_ns = SDL_GetTicksNS();
    bool running = true;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) running = false;
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
            {
                wvk::set_drawable_extent(device, {static_cast<wvk::uint32>(event.window.data1),
                                                  static_cast<wvk::uint32>(event.window.data2)});
            }
        }

        if (!running) break;

        if (wvk::get_device_error(device) != wvk::Error::none) {
            report("device lost");
            break;
        }

        // Throttle to frames_in_flight before touching anything the GPU might still be reading.
        if (frame > frames_in_flight) {
            wvk::wait_timeline({.semaphore = timeline, .value = frame - frames_in_flight});
        }

        const wvk::SwapchainFrame swapchain = wvk::acquire(device);
        if (!swapchain.render_view) {
            SDL_Delay(1);
            continue;
        }

        wvk::CommandBuffer* commands = wvk::begin_commands(device);
        if (!commands) break;

        const wvk::ColorAttachment colors[]{{
            .render_view = swapchain.render_view,
            .load = wvk::LoadOp::clear,
            .store = wvk::StoreOp::store,
            .clear = {.x = 0.02f, .y = 0.02f, .z = 0.04f, .w = 1.0f},
        }};
        wvk::begin_render_pass(commands, {.colors = colors});

        wvk::bind_pso(commands, pso);

        const PushConstants root{
            .vertices = wvk::get_handle(vertex_view),
        };
        wvk::draw(commands, wvk::root_of(root), 3);

        wvk::end_render_pass(commands);

        frame++;
        wvk::submit_and_present(device, {&commands, 1}, {.semaphore = timeline, .value = frame});
    }

    wvk::wait_idle(device);
    wvk::destroy_buffer_view(vertex_view);
    wvk::destroy_buffer(vertices);
    wvk::destroy_pso(pso);
    wvk::destroy_timeline_semaphore(timeline);
    wvk::destroy_device(device);

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
