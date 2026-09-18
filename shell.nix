let pkgs = import <nixpkgs> {};
in pkgs.mkShell {
  buildInputs = with pkgs; [
    xmake

    sdl3
    pkg-config
    directx-shader-compiler
    vulkan-loader
    vulkan-validation-layers
    vulkan-caps-viewer
    vulkan-tools
    alsa-lib
  ];

  VK_LAYER_PATH = "${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d";

  LD_LIBRARY_PATH="$LD_LIBRARY_PATH:${
    with pkgs;
    pkgs.lib.makeLibraryPath [
      directx-shader-compiler
      sdl3
      vulkan-loader
      alsa-lib
    ]
  }";
}
