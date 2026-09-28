add_requires("libsdl3")

target("imgui")
    set_kind("binary")
    set_group("examples")
    -- wvk_imgui brings the imgui package (built with its SDL3 platform backend when the
    -- examples are enabled, see the root xmake.lua).
    add_deps("wvk", "wvk_imgui")
    add_packages("libsdl3")

    add_files("main.cpp")
    if is_plat("linux") then
        add_defines("GAME_LINUX=1")
    end

    -- Only the scene needs compiling; the ImGui shaders are embedded in the backend.
    add_rules("hlsl2spv")
    add_files("scene.hlsl", {stages = {vertex = "vertex_main", fragment = "pixel_main"}})
