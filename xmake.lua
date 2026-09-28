-- Standalone:
--   xmake f -m release [--wvk-examples=y] [--wvk-imgui=y]
--   xmake
--   xmake run triangle
--
-- As a subproject (git submodule or a plain copy, in any folder):
--   includes("third_party/wvk")
--   target("game")
--       add_deps("wvk")
--       add_deps("wvk_imgui")   -- optional, needs --wvk-imgui=y, see imgui/imgui_impl_wvk.h
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

option("wvk-imgui")
    set_default(false)
    set_showmenu(true)
    set_description("Build the Dear ImGui renderer backend (target wvk_imgui); implied by --wvk-examples")
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

-- Dear ImGui renderer backend. The SPIR-V is embedded, so this needs no dxc.
--
-- wvk requires the xmake-repo "imgui" package, with the SDL3 platform backend when the examples are
-- built. An including project that wants another version or other backends overrides it with
--   add_requireconfs("imgui", {version = "v1.92.7-docking", configs = {sdl3 = true}})
-- rather than a second add_requires("imgui"), so that only one copy of ImGui gets linked.
if has_config("wvk-imgui") or has_config("wvk-examples") then
    -- No config is pinned unless the examples need one: add_requireconfs cannot override a config
    -- that add_requires sets explicitly.
    if has_config("wvk-examples") then
        add_requires("imgui", {configs = {sdl3 = true}})
    else
        add_requires("imgui")
    end

    target("wvk_imgui")
        set_kind("static")
        set_languages("c++20", {public = true})
        set_warnings("all")

        add_deps("wvk")
        add_files("imgui/imgui_impl_wvk.cpp")
        add_includedirs("imgui", {public = true})
        add_headerfiles("imgui/imgui_impl_wvk.h")
        add_packages("imgui", {public = true})
    target_end()
end

if has_config("wvk-examples") then
    includes("examples/*")
end
