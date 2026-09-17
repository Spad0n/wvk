# wvk

A thin Vulkan 1.3 abstraction.

## Building

```sh
xmake f -m release [--wvk-examples=y]
xmake
xmake run triangle # one of the examples
```

| Option           | Values  | Default | Meaning        |
|------------------|---------|---------|----------------|
| `--wvk-examples` | `y` `n` | `n`     | build examples |

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
so shaders write `#include "shaders/wvk.hlsl"

Target values (all optional):

| Option                | Meaning                                                                    |
|-----------------------|----------------------------------------------------------------------------|
| hlsl2spv.includedirs  | extra -I directories; relative paths are taken from the target's xmake.lua |
| hlsl2spv.outputdir    | subdirectory of the target directory for the .spv files (default: none)    |
| hlsl2spv.shader_model | default "6_6", the minimum for ResourceDescriptorHeap                      |
| hlsl2spv.flags        | extra dxc flags                                                            |
