{
  description = "RenderingOut Linux development environment";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { nixpkgs, ... }:
    let
      systems = [ "x86_64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      packages = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          ffmpeg-builder = pkgs.callPackage ./ffmpeg-builder/nixos.nix { };
        });

      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.mkShell {
            nativeBuildInputs = with pkgs; [
              cmake
              nasm
              ninja
              pkg-config
            ];

            buildInputs = with pkgs; [
              ffmpeg
              ffmpeg.dev
              libglvnd
              libglvnd.dev
            ];

            CMAKE_GENERATOR = "Ninja";

            shellHook = ''
              if [ -z "''${UNITY_PLUGIN_API:-}" ]; then
                echo "RenderingOut: set UNITY_PLUGIN_API to Unity's Editor/Data/PluginAPI directory before configuring." >&2
              fi
            '';
          };
        });
    };
}
