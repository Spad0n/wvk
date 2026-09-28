// Frame structure:
//   wait for frame N - frames_in_flight -> acquire -> ImGui frame -> ImGui_ImplWVK_UpdateTextures
//   -> begin_commands -> render pass { scene, ImGui } -> submit_and_present

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_wvk.h>

#include <wvk/wvk.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

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

// Matches SceneRoot in scene.hlsl.
struct SceneRoot
{
    float aspect;
    float angle;
    float size;
    float padding;
    float tint[4];
};

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
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "wvk imgui", message, nullptr);
}

const char* format_name(wvk::Format format) {
    switch (format) {
    case wvk::Format::bgra8_unorm:   return "bgra8_unorm";
    case wvk::Format::bgra8_srgb:    return "bgra8_srgb";
    case wvk::Format::rgba8_unorm:   return "rgba8_unorm";
    case wvk::Format::rgba8_srgb:    return "rgba8_srgb";
    case wvk::Format::rgb10a2_unorm: return "rgb10a2_unorm";
    case wvk::Format::rgba16_float:  return "rgba16_float";
    default:                         return "other";
    }
}

int main() {
    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_VIDEO)) { report(SDL_GetError()); return 1; }
    defer(SDL_Quit());

    // Size the window and the UI for the display's content scale.
    const float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    SDL_Window* window = SDL_CreateWindow("wvk imgui", (int)(1280 * main_scale), (int)(800 * main_scale),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) { report(SDL_GetError()); return 1; }
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
    if (!init.device) { report("create_device failed"); return 1; }
    wvk::Device* device = init.device;
    defer(wvk::destroy_device(device));
    const wvk::DeviceCaps& caps = wvk::get_device_caps(device);

    // Scene PSO
    const std::vector<wvk::uint32> vertex_spirv   = read_spirv("scene.vert.spv");
    const std::vector<wvk::uint32> fragment_spirv = read_spirv("scene.frag.spv");
    if (vertex_spirv.empty() || fragment_spirv.empty()) {
        report("Could not read scene.vert.spv / scene.frag.spv");
        return 1;
    }
    const wvk::ColorTargetDesc color_targets[]{{.format = caps.swapchain_format}};
    wvk::PSO* scene_pso = wvk::create_graphics_pso(device, {
        .vertex_spirv   = vertex_spirv,
        .fragment_spirv = fragment_spirv,
        .color_targets  = color_targets,
    });
    defer(wvk::destroy_pso(scene_pso));

    // A user texture for ImGui::Image
    // Created before the first begin_commands, like any application texture.
    constexpr wvk::uint32 checker_size = 16;
    wvk::Texture* checker = wvk::create_texture(device, {
        .extent = {.x = checker_size, .y = checker_size, .z = 1},
        .format = wvk::Format::rgba8_unorm,
        .usage  = wvk::TextureUsage::sampled | wvk::TextureUsage::transfer_destination,
    });
    defer(wvk::destroy_texture(checker));
    wvk::TextureView* checker_view = wvk::create_texture_view(device, {.texture = checker});
    defer(wvk::destroy_texture_view(checker_view));
    wvk::Sampler* nearest = wvk::create_sampler(device, {
        .min_filter = wvk::Filter::nearest,
        .mag_filter = wvk::Filter::nearest,
        .mip_filter = wvk::Filter::nearest,
        .address_u  = wvk::AddressMode::repeat,
        .address_v  = wvk::AddressMode::repeat,
    });
    defer(wvk::destroy_sampler(nearest));

    wvk::TimelineSemaphore* timeline = wvk::create_timeline_semaphore(device);
    defer(wvk::destroy_timeline_semaphore(timeline));

    if (!scene_pso || !checker || !checker_view || !nearest || !timeline) {
        report("resource creation failed");
        return 1;
    }

    {
        wvk::Buffer* staging = wvk::create_buffer(device, {
            .byte_count = checker_size * checker_size * 4,
            .memory     = wvk::MemoryType::cpu_to_gpu,
        });
        defer(wvk::destroy_buffer(staging));
        wvk::uint8* pixels = wvk::mapped_pointer(staging);
        for (wvk::uint32 y = 0; y < checker_size; ++y)
        for (wvk::uint32 x = 0; x < checker_size; ++x) {
            const bool light = ((x / 4) + (y / 4)) & 1;
            wvk::uint8* p = pixels + (y * checker_size + x) * 4;
            p[0] = light ? 240 : 40;
            p[1] = light ? 200 : 40;
            p[2] = light ? 80  : 60;
            p[3] = 255;
        }
        wvk::CommandBuffer* upload = wvk::begin_commands(device);
        wvk::copy_buffer_to_texture(upload, {.buffer = staging}, checker);
        wvk::barrier(upload, wvk::Stage::transfer, wvk::Access::transfer_write,
                             wvk::Stage::fragment, wvk::Access::shader_read);
        wvk::submit(device, {&upload, 1}, {.semaphore = timeline, .value = 1});
        wvk::wait_timeline({.semaphore = timeline, .value = 1});
    }

    // Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    defer(ImGui::DestroyContext());
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    if (!ImGui_ImplSDL3_InitForVulkan(window)) { report("ImGui_ImplSDL3_InitForVulkan failed"); return 1; }
    defer(ImGui_ImplSDL3_Shutdown());

    ImGui_ImplWVK_InitInfo imgui_info = {};
    imgui_info.Device         = device;
    imgui_info.ColorFormat    = caps.swapchain_format;
    imgui_info.FramesInFlight = frames_in_flight;
    if (!ImGui_ImplWVK_Init(imgui_info)) { report("ImGui_ImplWVK_Init failed"); return 1; }
    defer(ImGui_ImplWVK_Shutdown());

    const ImTextureID checker_linear  = ImGui_ImplWVK_TextureID(checker_view);          // backend default sampler
    const ImTextureID checker_nearest = ImGui_ImplWVK_TextureID(checker_view, nearest); // our own sampler

    // UI state
    bool show_demo_window = true;
    bool nearest_filter   = true;
    bool spin             = true;
    float speed           = 1.0f;
    float size            = 1.0f;
    float angle           = 0.0f;
    ImVec4 tint           = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 clear_color    = ImVec4(0.06f, 0.06f, 0.09f, 1.0f);

    wvk::uint64 frame = 1;
    wvk::uint64 last_ticks = SDL_GetTicksNS();
    bool running = true;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window))
                running = false;
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                wvk::set_drawable_extent(device, {static_cast<wvk::uint32>(event.window.data1),
                                                  static_cast<wvk::uint32>(event.window.data2)});
        }
        if (!running) break;
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) { SDL_Delay(10); continue; }
        if (wvk::get_device_error(device) != wvk::Error::none) { report("device lost"); break; }

        // Frame pacing: this is the guarantee ImGui_ImplWVK_InitInfo::FramesInFlight relies on.
        if (frame > frames_in_flight)
            wvk::wait_timeline({.semaphore = timeline, .value = frame - frames_in_flight});

        const wvk::SwapchainFrame swapchain = wvk::acquire(device);
        if (!swapchain.render_view) { SDL_Delay(1); continue; }

        const wvk::uint64 now = SDL_GetTicksNS();
        const float dt = (float)(now - last_ticks) * 1e-9f;
        last_ticks = now;
        if (spin) angle = std::fmod(angle + dt * speed, 6.2831853f);

        // UI
        ImGui_ImplWVK_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        if (show_demo_window)
            ImGui::ShowDemoWindow(&show_demo_window);

        ImGui::SetNextWindowPos(ImVec2(20.0f * main_scale, 20.0f * main_scale), ImGuiCond_FirstUseEver);
        ImGui::Begin("wvk");
        ImGui::Text("Device: %s", caps.device_name.c_str());
        ImGui::Text("unified image layout: %s", caps.unified_image_layouts ? "true" : "false");
        ImGui::Text("Swapchain: %u x %u, %s", swapchain.extent.x, swapchain.extent.y, format_name(caps.swapchain_format));
        ImGui::Text("%.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
        ImGui::Checkbox("Demo window", &show_demo_window);

        ImGui::SeparatorText("Scene");
        ImGui::Checkbox("Spin", &spin);
        ImGui::SliderFloat("Speed", &speed, -5.0f, 5.0f);
        ImGui::SliderAngle("Angle", &angle, 0.0f, 360.0f);
        ImGui::SliderFloat("Size", &size, 0.1f, 2.0f);
        ImGui::ColorEdit4("Tint", &tint.x);
        ImGui::ColorEdit3("Clear", &clear_color.x);

        ImGui::SeparatorText("User texture");
        ImGui::Checkbox("Nearest sampler", &nearest_filter);
        const float image_size = 128.0f * main_scale;
        ImGui::Image(ImTextureRef(nearest_filter ? checker_nearest : checker_linear), ImVec2(image_size, image_size),
                     ImVec2(0.0f, 0.0f), ImVec2(2.0f, 2.0f));
        ImGui::End();

        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();

        // Font atlas creation and updates, submitted ahead of this frame's command buffer.
        ImGui_ImplWVK_UpdateTextures(draw_data);

        // Rendering
        wvk::CommandBuffer* commands = wvk::begin_commands(device);
        if (!commands) break;

        const wvk::ColorAttachment colors[]{{
            .render_view = swapchain.render_view,
            .load  = wvk::LoadOp::clear,
            .store = wvk::StoreOp::store,
            .clear = {.x = clear_color.x, .y = clear_color.y, .z = clear_color.z, .w = 1.0f},
        }};
        wvk::begin_render_pass(commands, {.colors = colors});

        const SceneRoot scene{
            .aspect = (float)swapchain.extent.y / (float)swapchain.extent.x,
            .angle  = angle,
            .size   = size,
            .tint   = {tint.x, tint.y, tint.z, tint.w},
        };
        wvk::bind_pso(commands, scene_pso);
        wvk::draw(commands, wvk::root_of(scene), 3);

        ImGui_ImplWVK_RenderDrawData(draw_data, commands);

        wvk::end_render_pass(commands);

        frame++;
        wvk::submit_and_present(device, {&commands, 1}, {.semaphore = timeline, .value = frame});
    }

    wvk::wait_idle(device);
    return 0;
}
