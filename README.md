# wvk

A thin Vulkan 1.3 abstraction.

## Building

```sh
xmake f -m release [--wvk-examples=y] [--wvk-imgui=y]
xmake
xmake run triangle # one of the examples: triangle, textures, imgui
```

| Option           | Values  | Default | Meaning                                                  |
|------------------|---------|---------|----------------------------------------------------------|
| `--wvk-examples` | `y` `n` | `n`     | build examples (implies `--wvk-imgui`)                   |
| `--wvk-imgui`    | `y` `n` | `n`     | build the Dear ImGui renderer backend (target `wvk_imgui`) |

`xmake project -k compile_commands` writes `compile_commands.json` for clangd.

## Using wvk from another project

### As a subproject (git submodule or a plain copy, in any folder)

```sh
git submodule add https://github.com/Spad0n/wvk third_party/wvk
```

```lua
includes("third_party/wvk")
target("game")
    add_deps("wvk")
    add_rules("hlsl2spv") -- optional, see xmake/rules/hlsl2spv.lua
```

### Using hlsl2spv from another project (optional)

```lua
target("game")
    add_deps("wvk")
    add_rules("hlsl2spv")
    add_files("shaders/lit.hlsl", {stages = {vertex = "vertex_main", fragment = "pixel_main"}})
```

One output per stage: `<targetdir>/<outputdir>/<basename>.<vert|frag|comp|mesh|task>.spv`
The wvk sourc root is added to the include path automatically when the target depends on wvk,
so shaders write `#include "shaders/wvk.hlsl"`.

Target values (all optional):

| Option                | Meaning                                                                    |
|-----------------------|----------------------------------------------------------------------------|
| hlsl2spv.includedirs  | extra -I directories; relative paths are taken from the target's xmake.lua |
| hlsl2spv.outputdir    | subdirectory of the target directory for the .spv files (default: none)    |
| hlsl2spv.shader_model | default "6_6", the minimum for ResourceDescriptorHeap                      |
| hlsl2spv.flags        | extra dxc flags                                                            |

## Dear ImGui backend

`imgui/imgui_impl_wvk.h` is a ready-to-use Dear ImGui (1.92+) renderer backend. Its shaders are
embedded as SPIR-V, so it needs neither dxc nor any `.spv` file at run time. It supports the dynamic
font atlas (`ImGuiBackendFlags_RendererHasTextures`), large meshes (`RendererHasVtxOffset`), user
textures, sRGB colour targets and draw callbacks. `examples/imgui` shows it with SDL3.

```lua
set_config("wvk-imgui", true)   -- or pass --wvk-imgui=y to xmake f
includes("third_party/wvk")

-- The platform backend is up to you, e.g. SDL3 (imgui_impl_sdl3.h):
add_requireconfs("imgui", {configs = {sdl3 = true}})
add_requires("libsdl3")

target("game")
    add_deps("wvk", "wvk_imgui")   -- wvk_imgui also exports the imgui package
    add_packages("libsdl3")
```

wvk requires the xmake-repo `imgui` package itself, so configure it with `add_requireconfs("imgui", ...)`
(before or after the `includes`) rather than a second `add_requires("imgui")`, which would link two
copies of ImGui. A version can be pinned the same way:
`add_requireconfs("imgui", {version = "v1.92.7-docking", configs = {sdl3 = true}})`.

A frame looks like this; see the comments in `imgui/imgui_impl_wvk.h` for the details.

```cpp
ImGui_ImplWVK_InitInfo info = {};
info.Device         = device;
info.ColorFormat    = wvk::get_device_caps(device).swapchain_format;
info.FramesInFlight = frames_in_flight; // must match your wait_timeline pacing
ImGui_ImplWVK_Init(info);

// Every frame, after acquire():
ImGui_ImplWVK_NewFrame();
ImGui_ImplSDL3_NewFrame();
ImGui::NewFrame();
// ... UI ...
ImGui::Render();
ImDrawData* draw_data = ImGui::GetDrawData();
ImGui_ImplWVK_UpdateTextures(draw_data);        // before begin_commands
wvk::CommandBuffer* commands = wvk::begin_commands(device);
wvk::begin_render_pass(commands, {.colors = colors});
// ... scene ...
ImGui_ImplWVK_RenderDrawData(draw_data, commands);
wvk::end_render_pass(commands);
wvk::submit_and_present(device, {&commands, 1}, {.semaphore = timeline, .value = ++frame});
```

`ImGui_ImplWVK_UpdateTextures` must come before `begin_commands` because wvk initializes new textures
in the next command buffer begun. User textures go through
`ImGui_ImplWVK_TextureID(texture_view, sampler /* optional */)`.

After editing `imgui/imgui_impl_wvk.hlsl`, regenerate the embedded SPIR-V with
`xmake lua imgui/generate_spirv.lua`.
