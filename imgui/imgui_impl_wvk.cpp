// dear imgui: Renderer Backend for wvk
// See imgui_impl_wvk.h for the feature list and the expected frame structure.

#include "imgui_impl_wvk.h"
#ifndef IMGUI_DISABLE

#include <cstring>

#include "imgui_impl_wvk_spirv.inl"

static_assert(sizeof(ImTextureID) >= sizeof(ImU64), "imgui_impl_wvk packs a texture and a sampler handle into ImTextureID");

//-----------------------------------------------------------------------------------------------
// DATA
//-----------------------------------------------------------------------------------------------

// Must match ImGuiRoot in imgui_impl_wvk.hlsl.
struct ImGui_ImplWVK_Root
{
    float scale[2];
    float translate[2];
    uint32_t vertex_offset;
    wvk::ResourceHandle vertices;
    wvk::ResourceHandle texture;
    wvk::ResourceHandle texture_sampler;
    uint32_t flags;
};
static_assert(sizeof(ImGui_ImplWVK_Root) == 36);

static constexpr uint32_t ImGui_ImplWVK_Flag_SrgbTarget = 1u;

static bool ImGui_ImplWVK_IsSrgb(wvk::Format format)
{
    switch (format)
    {
    case wvk::Format::r8_srgb:
    case wvk::Format::rg8_srgb:
    case wvk::Format::rgba8_srgb:
    case wvk::Format::bgra8_srgb:
        return true;
    default:
        return false;
    }
}

// Backend storage for one ImTextureData, in ImTextureData::BackendUserData.
struct ImGui_ImplWVK_Texture
{
    wvk::Texture* Texture = nullptr;
    wvk::TextureView* View = nullptr;
};

// Vertex and index storage for one frame in flight. Both are persistently mapped cpu_to_gpu buffers
// that grow on demand; a slot is only rewritten FramesInFlight frames after its last use.
struct ImGui_ImplWVK_FrameBuffers
{
    wvk::Buffer* Vertices = nullptr;
    wvk::BufferView* VertexView = nullptr;
    uint64_t VertexCapacity = 0;
    wvk::Buffer* Indices = nullptr;
    uint64_t IndexCapacity = 0;
};

// A staging buffer that is freed once the upload submission covering it has completed.
struct ImGui_ImplWVK_Staging
{
    wvk::Buffer* Buffer = nullptr;
    uint64_t RetireValue = 0;
};

struct ImGui_ImplWVK_Data
{
    ImGui_ImplWVK_InitInfo InitInfo;
    ImVector<uint32_t> VertexSpirv;   // Owned copies, so the PSO can be rebuilt later.
    ImVector<uint32_t> FragmentSpirv;

    wvk::PSO* PSO = nullptr;
    wvk::Sampler* Sampler = nullptr;

    // Signalled by every texture upload submission; frees staging buffers without a CPU wait.
    wvk::TimelineSemaphore* UploadTimeline = nullptr;
    uint64_t UploadValue = 0;
    ImVector<ImGui_ImplWVK_Staging> Staging;

    ImVector<ImGui_ImplWVK_FrameBuffers> Frames;
    uint32_t FrameIndex = 0;
};

// Backend data stored in io.BackendRendererUserData to allow support for multiple Dear ImGui
// contexts. Multiple contexts sharing one wvk device are fine; each gets its own buffers.
static ImGui_ImplWVK_Data* ImGui_ImplWVK_GetBackendData()
{
    return ImGui::GetCurrentContext() ? (ImGui_ImplWVK_Data*)ImGui::GetIO().BackendRendererUserData : nullptr;
}

//-----------------------------------------------------------------------------------------------
// TEXTURE IDS
//-----------------------------------------------------------------------------------------------

// ImTextureID layout: bits 0-31 the texture view's bindless handle, bits 32-62 the sampler handle
// plus one (zero selects the backend's default sampler), bit 63 always set so that no valid ID can
// collide with ImTextureID_Invalid (zero).
static constexpr ImU64 ImGui_ImplWVK_TextureIDMarker = 1ull << 63;

static ImTextureID ImGui_ImplWVK_EncodeTextureID(wvk::ResourceHandle texture, wvk::ResourceHandle sampler)
{
    const ImU64 sampler_bits = sampler == wvk::invalid_handle ? 0 : ((ImU64)sampler + 1) & 0x7fffffffull;
    return (ImTextureID)(ImGui_ImplWVK_TextureIDMarker | (sampler_bits << 32) | (ImU64)texture);
}

static void ImGui_ImplWVK_DecodeTextureID(ImTextureID id, wvk::ResourceHandle default_sampler,
                                          wvk::ResourceHandle* texture, wvk::ResourceHandle* sampler)
{
    const ImU64 bits = (ImU64)id;
    const ImU64 sampler_bits = (bits >> 32) & 0x7fffffffull;
    *texture = (wvk::ResourceHandle)(bits & 0xffffffffull);
    *sampler = sampler_bits == 0 ? default_sampler : (wvk::ResourceHandle)(sampler_bits - 1);
}

ImTextureID ImGui_ImplWVK_TextureID(const wvk::TextureView* view, const wvk::Sampler* sampler)
{
    IM_ASSERT(view != nullptr);
    return ImGui_ImplWVK_EncodeTextureID(wvk::get_handle(view), sampler ? wvk::get_handle(sampler) : wvk::invalid_handle);
}

//-----------------------------------------------------------------------------------------------
// TEXTURES
//-----------------------------------------------------------------------------------------------

static void ImGui_ImplWVK_RetireStaging(ImGui_ImplWVK_Data* bd)
{
    if (bd->Staging.empty() || !bd->UploadTimeline)
        return;
    const uint64_t completed = wvk::completed_value(bd->UploadTimeline);
    for (int n = 0; n < bd->Staging.Size;)
    {
        if (bd->Staging[n].RetireValue <= completed)
        {
            wvk::destroy_buffer(bd->Staging[n].Buffer);
            bd->Staging.erase_unsorted(&bd->Staging[n]);
        }
        else
        {
            ++n;
        }
    }
}

static void ImGui_ImplWVK_DestroyTexture(ImTextureData* tex)
{
    if (ImGui_ImplWVK_Texture* backend_tex = (ImGui_ImplWVK_Texture*)tex->BackendUserData)
    {
        if (backend_tex->View)
            wvk::destroy_texture_view(backend_tex->View);
        if (backend_tex->Texture)
            wvk::destroy_texture(backend_tex->Texture);
        IM_DELETE(backend_tex);
        tex->BackendUserData = nullptr;
    }
    tex->SetTexID(ImTextureID_Invalid);
    tex->SetStatus(ImTextureStatus_Destroyed);
}

// Creates the wvk texture for a WantCreate request. Must run before the upload command buffer is
// begun, so that begin_commands records its UNDEFINED -> GENERAL transition.
static bool ImGui_ImplWVK_CreateTexture(ImGui_ImplWVK_Data* bd, ImTextureData* tex)
{
    IM_ASSERT(tex->BackendUserData == nullptr && tex->TexID == ImTextureID_Invalid);
    IM_ASSERT(tex->Format == ImTextureFormat_RGBA32 || tex->Format == ImTextureFormat_Alpha8);
    wvk::Device* device = bd->InitInfo.Device;

    // Alpha8 atlases are expanded to white RGBA on upload, so every ImGui texture shares one format
    // and one shader path.
    wvk::Texture* texture = wvk::create_texture(device, {
        .type = wvk::TextureType::two_d,
        .extent = {.x = (uint32_t)tex->Width, .y = (uint32_t)tex->Height, .z = 1},
        .format = wvk::Format::rgba8_unorm,
        .usage = wvk::TextureUsage::sampled | wvk::TextureUsage::transfer_destination,
    });
    if (!texture)
        return false;
    wvk::TextureView* view = wvk::create_texture_view(device, {.texture = texture});
    if (!view)
    {
        wvk::destroy_texture(texture);
        return false;
    }

    ImGui_ImplWVK_Texture* backend_tex = IM_NEW(ImGui_ImplWVK_Texture)();
    backend_tex->Texture = texture;
    backend_tex->View = view;
    tex->BackendUserData = backend_tex;
    tex->SetTexID(ImGui_ImplWVK_EncodeTextureID(wvk::get_handle(view), wvk::invalid_handle));
    return true;
}

// Copies one rectangle of an ImTextureData into staging memory as tightly packed RGBA8.
static void ImGui_ImplWVK_PackRect(ImTextureData* tex, int x, int y, int w, int h, uint8_t* dst)
{
    for (int row = 0; row < h; ++row)
    {
        const uint8_t* src = (const uint8_t*)tex->GetPixelsAt(x, y + row);
        uint8_t* out = dst + (size_t)row * (size_t)w * 4;
        if (tex->Format == ImTextureFormat_RGBA32)
        {
            memcpy(out, src, (size_t)w * 4);
        }
        else
        {
            for (int col = 0; col < w; ++col)
            {
                out[col * 4 + 0] = 255;
                out[col * 4 + 1] = 255;
                out[col * 4 + 2] = 255;
                out[col * 4 + 3] = src[col];
            }
        }
    }
}

void ImGui_ImplWVK_UpdateTextures(ImDrawData* draw_data)
{
    ImGui_ImplWVK_Data* bd = ImGui_ImplWVK_GetBackendData();
    IM_ASSERT(bd != nullptr && "Did you call ImGui_ImplWVK_Init()?");
    if (!draw_data || !draw_data->Textures)
        return;
    wvk::Device* device = bd->InitInfo.Device;
    if (wvk::get_device_error(device) != wvk::Error::none)
        return;

    ImGui_ImplWVK_RetireStaging(bd);

    // Pass 1: destroy, create, and size the staging buffer. Creation happens here, before the upload
    // command buffer is begun, because that is where wvk transitions new textures.
    uint64_t staging_bytes = 0;
    for (ImTextureData* tex : *draw_data->Textures)
    {
        switch (tex->Status)
        {
        case ImTextureStatus_WantDestroy:
            // A texture ImGui stopped using may still be sampled by frames in flight.
            if (tex->UnusedFrames > (int)bd->InitInfo.FramesInFlight)
                ImGui_ImplWVK_DestroyTexture(tex);
            break;
        case ImTextureStatus_WantCreate:
            if (ImGui_ImplWVK_CreateTexture(bd, tex))
                staging_bytes += (uint64_t)tex->Width * (uint64_t)tex->Height * 4;
            break;
        case ImTextureStatus_WantUpdates:
            for (const ImTextureRect& r : tex->Updates)
                staging_bytes += (uint64_t)r.w * (uint64_t)r.h * 4;
            break;
        default:
            break;
        }
    }
    if (staging_bytes == 0)
        return;

    wvk::Buffer* staging = wvk::create_buffer(device, {.byte_count = staging_bytes, .memory = wvk::MemoryType::cpu_to_gpu});
    if (!staging)
        return;
    uint8_t* staging_ptr = wvk::mapped_pointer(staging);

    // Pass 2: record the copies. Begun only now, after every new texture exists.
    wvk::CommandBuffer* commands = wvk::begin_commands(device);
    if (!commands)
    {
        wvk::destroy_buffer(staging);
        return;
    }

    // Write-after-read: an updated texture may still be sampled by an earlier submission. Pipeline
    // barriers reach back across submissions on wvk's single queue, so an execution dependency here
    // is all that is needed.
    wvk::barrier(commands, wvk::Stage::fragment, wvk::Access::none, wvk::Stage::transfer, wvk::Access::transfer_write);

    uint64_t offset = 0;
    for (ImTextureData* tex : *draw_data->Textures)
    {
        ImGui_ImplWVK_Texture* backend_tex = (ImGui_ImplWVK_Texture*)tex->BackendUserData;
        if (!backend_tex)
            continue;

        if (tex->Status == ImTextureStatus_WantCreate)
        {
            ImGui_ImplWVK_PackRect(tex, 0, 0, tex->Width, tex->Height, staging_ptr + offset);
            wvk::copy_buffer_to_texture(commands, {.buffer = staging, .offset = offset}, backend_tex->Texture);
            offset += (uint64_t)tex->Width * (uint64_t)tex->Height * 4;
            tex->SetStatus(ImTextureStatus_OK);
        }
        else if (tex->Status == ImTextureStatus_WantUpdates)
        {
            for (const ImTextureRect& r : tex->Updates)
            {
                ImGui_ImplWVK_PackRect(tex, r.x, r.y, r.w, r.h, staging_ptr + offset);
                wvk::copy_buffer_to_texture(commands, {.buffer = staging, .offset = offset}, backend_tex->Texture, {
                    .offset = {.x = r.x, .y = r.y, .z = 0},
                    .extent = {.x = r.w, .y = r.h, .z = 1},
                });
                offset += (uint64_t)r.w * (uint64_t)r.h * 4;
            }
            tex->SetStatus(ImTextureStatus_OK);
        }
    }
    IM_ASSERT(offset == staging_bytes);

    wvk::barrier(commands, wvk::Stage::transfer, wvk::Access::transfer_write, wvk::Stage::fragment, wvk::Access::shader_read);
    bd->UploadValue++;
    wvk::submit(device, {&commands, 1}, {.semaphore = bd->UploadTimeline, .value = bd->UploadValue});

    ImGui_ImplWVK_Staging retired;
    retired.Buffer = staging;
    retired.RetireValue = bd->UploadValue;
    bd->Staging.push_back(retired);
}

//-----------------------------------------------------------------------------------------------
// RENDERING
//-----------------------------------------------------------------------------------------------

// Grows one frame's vertex and index buffers to fit the draw data. Returns false on allocation
// failure, in which case the frame is skipped.
static bool ImGui_ImplWVK_EnsureFrameBuffers(ImGui_ImplWVK_Data* bd, ImGui_ImplWVK_FrameBuffers* fr,
                                             uint64_t vertex_bytes, uint64_t index_bytes)
{
    wvk::Device* device = bd->InitInfo.Device;

    // The slot being replaced was last used FramesInFlight frames ago, so it is safe to free now,
    // descriptor slot included.
    if (vertex_bytes > fr->VertexCapacity)
    {
        if (fr->VertexView)
            wvk::destroy_buffer_view(fr->VertexView);
        if (fr->Vertices)
            wvk::destroy_buffer(fr->Vertices);
        fr->VertexView = nullptr;
        fr->VertexCapacity = 0;

        const uint64_t capacity = vertex_bytes + vertex_bytes / 2 + sizeof(ImDrawVert) * 1024;
        fr->Vertices = wvk::create_buffer(device, {.byte_count = capacity, .memory = wvk::MemoryType::cpu_to_gpu});
        if (!fr->Vertices)
            return false;
        fr->VertexView = wvk::create_buffer_view(device, {.buffer = fr->Vertices, .type = wvk::BufferViewType::structured});
        if (!fr->VertexView)
            return false;
        fr->VertexCapacity = capacity;
    }

    if (index_bytes > fr->IndexCapacity)
    {
        if (fr->Indices)
            wvk::destroy_buffer(fr->Indices);
        fr->IndexCapacity = 0;

        uint64_t capacity = index_bytes + index_bytes / 2 + sizeof(ImDrawIdx) * 1024;
        capacity = (capacity + 3) & ~(uint64_t)3;
        fr->Indices = wvk::create_buffer(device, {
            .byte_count = capacity,
            .usage = wvk::BufferUsage::index,
            .memory = wvk::MemoryType::cpu_to_gpu,
        });
        if (!fr->Indices)
            return false;
        fr->IndexCapacity = capacity;
    }
    return true;
}

static void ImGui_ImplWVK_SetupRenderState(ImGui_ImplWVK_Data* bd, wvk::CommandBuffer* commands, int fb_width, int fb_height)
{
    wvk::bind_pso(commands, bd->PSO);
    wvk::set_viewport(commands, 0.0f, 0.0f, (float)fb_width, (float)fb_height);
    wvk::set_depth_stencil(commands, {});
}

void ImGui_ImplWVK_RenderDrawData(ImDrawData* draw_data, wvk::CommandBuffer* commands)
{
    ImGui_ImplWVK_Data* bd = ImGui_ImplWVK_GetBackendData();
    IM_ASSERT(bd != nullptr && "Did you call ImGui_ImplWVK_Init()?");
    if (!draw_data || !commands || !bd->PSO)
        return;

    // Avoid rendering when minimized, scale coordinates for retina displays
    // (screen coordinates != framebuffer coordinates).
    const int fb_width = (int)(draw_data->DisplaySize.x * draw_data->FramebufferScale.x);
    const int fb_height = (int)(draw_data->DisplaySize.y * draw_data->FramebufferScale.y);
    if (fb_width <= 0 || fb_height <= 0)
        return;

    // Fallback for applications that did not call ImGui_ImplWVK_UpdateTextures() themselves.
    if (draw_data->Textures)
    {
        for (ImTextureData* tex : *draw_data->Textures)
        {
            if (tex->Status != ImTextureStatus_OK)
            {
                ImGui_ImplWVK_UpdateTextures(draw_data);
                break;
            }
        }
    }

    if (draw_data->TotalVtxCount <= 0 || draw_data->TotalIdxCount <= 0)
        return;

    ImGui_ImplWVK_FrameBuffers* fr = &bd->Frames[(int)bd->FrameIndex];
    const uint64_t vertex_bytes = (uint64_t)draw_data->TotalVtxCount * sizeof(ImDrawVert);
    const uint64_t index_bytes = (uint64_t)draw_data->TotalIdxCount * sizeof(ImDrawIdx);
    if (!ImGui_ImplWVK_EnsureFrameBuffers(bd, fr, vertex_bytes, index_bytes))
        return;

    // Upload vertices and indices into one contiguous stream each. The buffers are coherent and
    // persistently mapped, so nothing else is needed before the GPU reads them.
    {
        ImDrawVert* vtx_dst = (ImDrawVert*)wvk::mapped_pointer(fr->Vertices);
        ImDrawIdx* idx_dst = (ImDrawIdx*)wvk::mapped_pointer(fr->Indices);
        for (const ImDrawList* draw_list : draw_data->CmdLists)
        {
            memcpy(vtx_dst, draw_list->VtxBuffer.Data, (size_t)draw_list->VtxBuffer.Size * sizeof(ImDrawVert));
            memcpy(idx_dst, draw_list->IdxBuffer.Data, (size_t)draw_list->IdxBuffer.Size * sizeof(ImDrawIdx));
            vtx_dst += draw_list->VtxBuffer.Size;
            idx_dst += draw_list->IdxBuffer.Size;
        }
    }

    ImGui_ImplWVK_SetupRenderState(bd, commands, fb_width, fb_height);

    // Expose render state to draw callbacks.
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    ImGui_ImplWVK_RenderState render_state;
    render_state.Device = bd->InitInfo.Device;
    render_state.Commands = commands;
    render_state.PSO = bd->PSO;
    render_state.DefaultSampler = wvk::get_handle(bd->Sampler);
    platform_io.Renderer_RenderState = &render_state;

    // The orthographic projection maps DisplayPos (top left) to DisplayPos + DisplaySize. wvk flips
    // the viewport to keep D3D conventions, so clip-space Y points up.
    ImGui_ImplWVK_Root root = {};
    root.scale[0] = 2.0f / draw_data->DisplaySize.x;
    root.scale[1] = -2.0f / draw_data->DisplaySize.y;
    root.translate[0] = -1.0f - draw_data->DisplayPos.x * root.scale[0];
    root.translate[1] = 1.0f - draw_data->DisplayPos.y * root.scale[1];
    root.vertices = wvk::get_handle(fr->VertexView);
    root.flags = ImGui_ImplWVK_IsSrgb(bd->InitInfo.ColorFormat) ? ImGui_ImplWVK_Flag_SrgbTarget : 0u;

    const wvk::IndexType index_type = sizeof(ImDrawIdx) == 2 ? wvk::IndexType::uint16 : wvk::IndexType::uint32;
    const wvk::BufferRange indices = {.buffer = fr->Indices};

    // Project scissor/clipping rectangles into framebuffer space.
    const ImVec2 clip_off = draw_data->DisplayPos;
    const ImVec2 clip_scale = draw_data->FramebufferScale;

    uint32_t global_vtx_offset = 0;
    uint32_t global_idx_offset = 0;
    for (const ImDrawList* draw_list : draw_data->CmdLists)
    {
        for (int cmd_i = 0; cmd_i < draw_list->CmdBuffer.Size; cmd_i++)
        {
            const ImDrawCmd* pcmd = &draw_list->CmdBuffer[cmd_i];
            if (pcmd->UserCallback != nullptr)
            {
                // ImDrawCallback_ResetRenderState is a special value used by the user to request the
                // renderer to reset its render state.
                if (pcmd->UserCallback == ImDrawCallback_ResetRenderState)
                    ImGui_ImplWVK_SetupRenderState(bd, commands, fb_width, fb_height);
                else
                    pcmd->UserCallback(draw_list, pcmd);
                continue;
            }

            ImVec2 clip_min((pcmd->ClipRect.x - clip_off.x) * clip_scale.x, (pcmd->ClipRect.y - clip_off.y) * clip_scale.y);
            ImVec2 clip_max((pcmd->ClipRect.z - clip_off.x) * clip_scale.x, (pcmd->ClipRect.w - clip_off.y) * clip_scale.y);
            if (clip_min.x < 0.0f) clip_min.x = 0.0f;
            if (clip_min.y < 0.0f) clip_min.y = 0.0f;
            if (clip_max.x > (float)fb_width) clip_max.x = (float)fb_width;
            if (clip_max.y > (float)fb_height) clip_max.y = (float)fb_height;
            if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y || pcmd->ElemCount == 0)
                continue;

            wvk::set_scissor(commands, (int32_t)clip_min.x, (int32_t)clip_min.y,
                             (uint32_t)(clip_max.x - clip_min.x), (uint32_t)(clip_max.y - clip_min.y));

            ImGui_ImplWVK_DecodeTextureID(pcmd->GetTexID(), render_state.DefaultSampler, &root.texture, &root.texture_sampler);
            // Vertex pulling: the base vertex travels in the root rather than through vertexOffset,
            // so SV_VertexID means the same thing whatever dxc's base-vertex convention.
            root.vertex_offset = pcmd->VtxOffset + global_vtx_offset;
            wvk::draw_indexed(commands, wvk::root_of(root), indices, index_type, pcmd->ElemCount, 1,
                              pcmd->IdxOffset + global_idx_offset, 0, 0);
        }
        global_idx_offset += (uint32_t)draw_list->IdxBuffer.Size;
        global_vtx_offset += (uint32_t)draw_list->VtxBuffer.Size;
    }
    platform_io.Renderer_RenderState = nullptr;

    // Leave the pass the way begin_render_pass does: full viewport and scissor.
    wvk::set_viewport(commands, 0.0f, 0.0f, (float)fb_width, (float)fb_height);
    wvk::set_scissor(commands, 0, 0, (uint32_t)fb_width, (uint32_t)fb_height);
}

//-----------------------------------------------------------------------------------------------
// DEVICE OBJECTS
//-----------------------------------------------------------------------------------------------

static bool ImGui_ImplWVK_CreatePSO(ImGui_ImplWVK_Data* bd)
{
    wvk::Device* device = bd->InitInfo.Device;
    if (bd->PSO)
    {
        wvk::destroy_pso(bd->PSO);
        bd->PSO = nullptr;
    }

    // Premultiplied-alpha-friendly blending, as in the other official backends.
    const wvk::ColorTargetDesc color_targets[]{{
        .format = bd->InitInfo.ColorFormat,
        .blend = {
            .enabled = true,
            .color = {.source = wvk::BlendFactor::source_alpha, .destination = wvk::BlendFactor::one_minus_source_alpha},
            .alpha = {.source = wvk::BlendFactor::one, .destination = wvk::BlendFactor::one_minus_source_alpha},
        },
    }};
    bd->PSO = wvk::create_graphics_pso(device, {
        .vertex_spirv = {bd->VertexSpirv.Data, (size_t)bd->VertexSpirv.Size},
        .fragment_spirv = {bd->FragmentSpirv.Data, (size_t)bd->FragmentSpirv.Size},
        .color_targets = color_targets,
        .depth_format = bd->InitInfo.DepthFormat,
        .stencil_format = bd->InitInfo.StencilFormat,
        .topology = wvk::Topology::triangles,
        .rasterization = {.cull = wvk::CullMode::none},
    });
    return bd->PSO != nullptr;
}

bool ImGui_ImplWVK_CreateDeviceObjects()
{
    ImGui_ImplWVK_Data* bd = ImGui_ImplWVK_GetBackendData();
    IM_ASSERT(bd != nullptr && "No renderer backend initialized!");
    wvk::Device* device = bd->InitInfo.Device;

    if (!ImGui_ImplWVK_CreatePSO(bd))
        return false;

    if (!bd->Sampler)
    {
        bd->Sampler = wvk::create_sampler(device, {
            .min_filter = wvk::Filter::linear,
            .mag_filter = wvk::Filter::linear,
            .mip_filter = wvk::Filter::linear,
            .address_u = wvk::AddressMode::clamp_to_edge,
            .address_v = wvk::AddressMode::clamp_to_edge,
            .address_w = wvk::AddressMode::clamp_to_edge,
            .max_lod = 0.0f,
        });
        if (!bd->Sampler)
            return false;
    }

    if (!bd->UploadTimeline)
    {
        bd->UploadTimeline = wvk::create_timeline_semaphore(device, 0);
        bd->UploadValue = 0;
        if (!bd->UploadTimeline)
            return false;
    }

    bd->Frames.resize((int)bd->InitInfo.FramesInFlight);
    for (ImGui_ImplWVK_FrameBuffers& fr : bd->Frames)
        fr = ImGui_ImplWVK_FrameBuffers();
    bd->FrameIndex = 0;
    return true;
}

void ImGui_ImplWVK_DestroyDeviceObjects()
{
    ImGui_ImplWVK_Data* bd = ImGui_ImplWVK_GetBackendData();
    IM_ASSERT(bd != nullptr && "No renderer backend initialized!");
    wvk::Device* device = bd->InitInfo.Device;

    // Everything below may still be referenced by submitted work.
    wvk::wait_idle(device);

    // Destroy all textures, unless another context still holds a reference.
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        if (tex->RefCount == 1)
            ImGui_ImplWVK_DestroyTexture(tex);

    for (ImGui_ImplWVK_Staging& staging : bd->Staging)
        wvk::destroy_buffer(staging.Buffer);
    bd->Staging.clear();

    for (ImGui_ImplWVK_FrameBuffers& fr : bd->Frames)
    {
        if (fr.VertexView) wvk::destroy_buffer_view(fr.VertexView);
        if (fr.Vertices) wvk::destroy_buffer(fr.Vertices);
        if (fr.Indices) wvk::destroy_buffer(fr.Indices);
        fr = ImGui_ImplWVK_FrameBuffers();
    }

    if (bd->UploadTimeline) wvk::destroy_timeline_semaphore(bd->UploadTimeline);
    if (bd->Sampler) wvk::destroy_sampler(bd->Sampler);
    if (bd->PSO) wvk::destroy_pso(bd->PSO);
    bd->UploadTimeline = nullptr;
    bd->Sampler = nullptr;
    bd->PSO = nullptr;
}

void ImGui_ImplWVK_SetColorFormat(wvk::Format color_format)
{
    ImGui_ImplWVK_Data* bd = ImGui_ImplWVK_GetBackendData();
    IM_ASSERT(bd != nullptr && "No renderer backend initialized!");
    if (bd->InitInfo.ColorFormat == color_format && bd->PSO)
        return;
    wvk::wait_idle(bd->InitInfo.Device);
    bd->InitInfo.ColorFormat = color_format;
    ImGui_ImplWVK_CreatePSO(bd);
}

//-----------------------------------------------------------------------------------------------
// MAIN INTERFACE
//-----------------------------------------------------------------------------------------------

bool ImGui_ImplWVK_Init(const ImGui_ImplWVK_InitInfo& info)
{
    ImGuiIO& io = ImGui::GetIO();
    IMGUI_CHECKVERSION();
    IM_ASSERT(io.BackendRendererUserData == nullptr && "Already initialized a renderer backend!");
    IM_ASSERT(info.Device != nullptr);
    IM_ASSERT(info.ColorFormat != wvk::Format::undefined && "Set ColorFormat, usually to DeviceCaps::swapchain_format");
    IM_ASSERT(info.FramesInFlight >= 1);
    IM_ASSERT(info.VertexSpirv.empty() == info.FragmentSpirv.empty() && "Override both shaders or neither");

    ImGui_ImplWVK_Data* bd = IM_NEW(ImGui_ImplWVK_Data)();
    bd->InitInfo = info;
    bd->InitInfo.VertexSpirv = {};
    bd->InitInfo.FragmentSpirv = {};

    const std::span<const uint32_t> vertex_spirv = info.VertexSpirv.empty()
        ? std::span<const uint32_t>(imgui_impl_wvk_vertex_spirv) : info.VertexSpirv;
    const std::span<const uint32_t> fragment_spirv = info.FragmentSpirv.empty()
        ? std::span<const uint32_t>(imgui_impl_wvk_fragment_spirv) : info.FragmentSpirv;
    bd->VertexSpirv.resize((int)vertex_spirv.size());
    memcpy(bd->VertexSpirv.Data, vertex_spirv.data(), vertex_spirv.size_bytes());
    bd->FragmentSpirv.resize((int)fragment_spirv.size());
    memcpy(bd->FragmentSpirv.Data, fragment_spirv.data(), fragment_spirv.size_bytes());

    // Setup backend capabilities flags
    io.BackendRendererUserData = (void*)bd;
    io.BackendRendererName = "imgui_impl_wvk";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;  // We can honor the ImDrawCmd::VtxOffset field, allowing for large meshes.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;   // We can honor ImGuiPlatformIO::Textures[] requests during render.

    if (!ImGui_ImplWVK_CreateDeviceObjects())
    {
        ImGui_ImplWVK_Shutdown();
        return false;
    }
    return true;
}

void ImGui_ImplWVK_Shutdown()
{
    ImGui_ImplWVK_Data* bd = ImGui_ImplWVK_GetBackendData();
    IM_ASSERT(bd != nullptr && "No renderer backend to shutdown, or already shutdown?");
    ImGuiIO& io = ImGui::GetIO();

    ImGui_ImplWVK_DestroyDeviceObjects();

    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    IM_DELETE(bd);
}

void ImGui_ImplWVK_NewFrame()
{
    ImGui_ImplWVK_Data* bd = ImGui_ImplWVK_GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplWVK_Init()?");

    // Advance the ring of per-frame buffers. The application's pacing guarantees the slot we move to
    // was last read by a frame that has completed.
    bd->FrameIndex = (bd->FrameIndex + 1) % (uint32_t)bd->Frames.Size;
    ImGui_ImplWVK_RetireStaging(bd);

    if (!bd->PSO)
        ImGui_ImplWVK_CreateDeviceObjects();
}

#endif // #ifndef IMGUI_DISABLE
