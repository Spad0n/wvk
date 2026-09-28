// dear imgui: Renderer Backend for wvk (a thin Vulkan 1.3 abstraction)
// This needs to be used along with a Platform Backend (e.g. imgui_impl_sdl3).
//
// Implemented features:
//  [X] Renderer: User texture binding. Use ImGui_ImplWVK_TextureID() to make an ImTextureID from a
//      wvk::TextureView, optionally paired with a wvk::Sampler.
//  [X] Renderer: Large meshes support (64k+ vertices) even with 16-bit indices
//      (ImGuiBackendFlags_RendererHasVtxOffset).
//  [X] Renderer: Texture updates support for dynamic font atlas (ImGuiBackendFlags_RendererHasTextures).
//  [X] Renderer: Expose selected render state for draw callbacks to use. Access in
//      '(ImGui_ImplWVK_RenderState*)GetPlatformIO().Renderer_RenderState'.
//  [X] Renderer: sRGB colour targets (vertex colours are linearized in the shader).
//  [ ] Renderer: Multi-viewport support. wvk has one swapchain per device.
//
// The shaders are embedded as SPIR-V (see imgui_impl_wvk.hlsl), so there is nothing to compile or
// ship alongside the executable.
//
// Typical frame, matching the wvk examples:
//
//     wvk::wait_timeline({timeline, frame - frames_in_flight});
//     const wvk::SwapchainFrame swapchain = wvk::acquire(device);
//     ImGui_ImplWVK_NewFrame();
//     ImGui_ImplSDL3_NewFrame();
//     ImGui::NewFrame();
//     ... build the UI ...
//     ImGui::Render();
//     ImDrawData* draw_data = ImGui::GetDrawData();
//     ImGui_ImplWVK_UpdateTextures(draw_data);        // Before begin_commands; see below.
//     wvk::CommandBuffer* commands = wvk::begin_commands(device);
//     wvk::begin_render_pass(commands, {...});
//     ... the scene ...
//     ImGui_ImplWVK_RenderDrawData(draw_data, commands);
//     wvk::end_render_pass(commands);
//     wvk::submit_and_present(device, {&commands, 1}, {timeline, ++frame});

#pragma once
#include "imgui.h"
#ifndef IMGUI_DISABLE

#include <wvk/wvk.hpp>

#include <cstdint>
#include <span>

struct ImGui_ImplWVK_InitInfo
{
    wvk::Device* Device = nullptr;

    // Format of the colour attachment ImGui is drawn into, usually DeviceCaps::swapchain_format.
    // An *_srgb format turns on colour linearization in the shader.
    wvk::Format ColorFormat = wvk::Format::undefined;

    // Set these when the render pass ImGui is drawn in also has a depth and/or stencil attachment;
    // the PSO must declare the same formats. Depth and stencil tests are always disabled for ImGui.
    wvk::Format DepthFormat = wvk::Format::undefined;
    wvk::Format StencilFormat = wvk::Format::undefined;

    // Must match the application's frame pacing: before ImGui_ImplWVK_NewFrame() for frame N, the
    // GPU work of frame N - FramesInFlight has completed (the wait_timeline pattern above). Vertex
    // and index buffers are ring-buffered this deep, and textures ImGui releases are destroyed only
    // after this many frames without use.
    uint32_t FramesInFlight = 2;

    // Optional replacement shaders, compiled with the flags of xmake/rules/hlsl2spv.lua and the
    // push-constant layout of imgui_impl_wvk.hlsl. Empty uses the embedded SPIR-V. The memory only
    // needs to live through ImGui_ImplWVK_Init() and ImGui_ImplWVK_SetColorFormat().
    std::span<const uint32_t> VertexSpirv = {};
    std::span<const uint32_t> FragmentSpirv = {};
};

// Handed to draw callbacks through ImGuiPlatformIO::Renderer_RenderState, valid only while
// ImGui_ImplWVK_RenderDrawData() runs. A callback may change any command state; the backend restores
// its own PSO, viewport and depth state when it sees ImDrawCallback_ResetRenderState.
struct ImGui_ImplWVK_RenderState
{
    wvk::Device* Device;
    wvk::CommandBuffer* Commands;
    const wvk::PSO* PSO;
    wvk::ResourceHandle DefaultSampler;
};

// Follow "Getting Started" link and check examples/ folder to learn about using backends!
IMGUI_IMPL_API bool ImGui_ImplWVK_Init(const ImGui_ImplWVK_InitInfo& info);
IMGUI_IMPL_API void ImGui_ImplWVK_Shutdown();
IMGUI_IMPL_API void ImGui_ImplWVK_NewFrame();

// Creates, updates and destroys the textures ImGui asked for (font atlas and the like), recording
// uploads into a command buffer of the backend's own and submitting it immediately. The submission
// is ordered on the queue before the application's frame, and no CPU wait is involved.
//
// Call it after ImGui::Render() and BEFORE wvk::begin_commands() for the frame: wvk initializes new
// textures in the first command buffer begun after they are created, and every begun command buffer
// must appear in the next submit. ImGui_ImplWVK_RenderDrawData() calls it anyway if it was skipped,
// which works but uses an internal submit while the application's command buffer is still recording.
IMGUI_IMPL_API void ImGui_ImplWVK_UpdateTextures(ImDrawData* draw_data);

// Records the ImGui draws. Call it inside a render pass whose colour attachment matches
// InitInfo::ColorFormat. On return the viewport and scissor cover the whole framebuffer again.
IMGUI_IMPL_API void ImGui_ImplWVK_RenderDrawData(ImDrawData* draw_data, wvk::CommandBuffer* commands);

// Makes an ImTextureID for ImGui::Image() and friends. A null sampler selects the backend's linear,
// clamp-to-edge sampler. The view must be a TextureViewType::sampled view of a 2D texture, and must
// stay alive until the frames that draw it have completed.
IMGUI_IMPL_API ImTextureID ImGui_ImplWVK_TextureID(const wvk::TextureView* view, const wvk::Sampler* sampler = nullptr);

// Rebuilds the PSO against a new colour format, e.g. after switching to an sRGB or HDR swapchain.
// Call it outside of ImGui_ImplWVK_RenderDrawData, with no frame using the old PSO in flight.
IMGUI_IMPL_API void ImGui_ImplWVK_SetColorFormat(wvk::Format color_format);

// Called by Init/Shutdown; exposed to force a rebuild. Destroying waits for the device to go idle.
IMGUI_IMPL_API bool ImGui_ImplWVK_CreateDeviceObjects();
IMGUI_IMPL_API void ImGui_ImplWVK_DestroyDeviceObjects();

#endif // #ifndef IMGUI_DISABLE
