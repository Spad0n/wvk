add_requires("libsdl3")

target("textures")
   set_kind("binary")
   set_group("examples")
   add_deps("wvk")
   add_packages("libsdl3")

   add_files("main.cpp")
   if is_plat("linux") then
      add_defines("GAME_LINUX=1")
   end

   -- add_files("texture.png")
   add_rules("hlsl2spv")
   add_files("quad.hlsl", {
      stages = {
	 vertex   = "vertex_main",
	 fragment = "pixel_main",
      }
   })

   after_build(function (target)
      local texture_path = path.join(os.scriptdir(), "texture.png")
      if os.isfile(texture_path) then
	 os.cp(texture_path, target:targetdir())
      end
   end)
