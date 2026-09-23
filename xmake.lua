-- Standalone:
--   xmake f -m release [--wvk_examples=y]
--   xmake
--   xmake run triangle
--
-- As a subproject (git submodule or a plain copy, in any folder):
--   includes("third_party/wvk")
--   target("game")
--       add_deps("wvk")
--       add_rules("hlsl2spv")   -- optional, see xmake/rules/hlsl2spv.lua

set_xmakever("2.8.5")

-- Only when wvk is the top-level project. When a game includes it, the game's project name,
-- version and build modes stay in charge.
if os.projectdir() == os.scriptdir() then
    set_project("wvk")
    set_version("0.1.0")
    add_rules("mode.debug", "mode.release", "mode.asan")
end

option("wvk-examples")
    set_default(false)
    set_showmenu(true)
    set_description("Build the wvk examples (needs SDL3 and a recent dxc)")
option_end()

-- VULKAN_SDK / VK_SDK_PATH first, then pkg-config vulkan, then /usr (Linux).
add_requires("vulkansdk")

-- HLSL -> SPIR-V rule, also available to the including project as add_rules("hlsl2spv").
includes("xmake/rules/hlsl2spv.lua")

target("wvk")
    -- The public header has no export annotations, so wvk is always a static library.
    set_kind("static")
    set_languages("c++20", {public = true})
    set_warnings("all")

    add_files("src/wvk.cpp")
    add_includedirs("include", {public = true})
    add_headerfiles("include/(wvk/*.hpp)")

    -- Public so that executables linking the static library also link the Vulkan loader.
    add_packages("vulkansdk", {public = true})

    if is_plat("windows") then
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
    end
target_end()

if has_config("wvk-examples") then
    includes("examples/*")
end
