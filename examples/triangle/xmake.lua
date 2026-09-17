add_requires("libsdl3")

target("triangle")
    set_kind("binary")
    set_group("examples")
    add_deps("wvk")
    add_packages("libsdl3")

    add_files("triangle.cpp")
    if is_plat("linux") then
        add_defines("GAME_LINUX=1")
    end

    -- Produces triangle.vert.spv and triangle.frag.spv next to the executable, which is also
    -- the working directory of `xmake run triangle`.
    add_rules("hlsl2spv")
    add_files("triangle.hlsl", {stages = {vertex = "vertex_main", fragment = "pixel_main"}})
