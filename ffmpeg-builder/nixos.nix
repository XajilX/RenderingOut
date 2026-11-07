{ pkgs ? import <nixpkgs> { } }:

# build.sh runs the upstream x264 configure script, whose shebang is
# "#!/bin/bash". NixOS has no /bin/bash, so run the unchanged build script in a
# standard FHS environment where /bin/bash and the usual build paths exist.
let
  runner = pkgs.writeShellScript "renderingout-ffmpeg-builder-run" ''
    set -eu

    if [ "$#" -eq 0 ]; then
      echo "Usage: renderingout-ffmpeg-builder <build.sh> [build.sh arguments...]" >&2
      exit 2
    fi

    script=$1
    shift

    case "$script" in
      /*) ;;
      *) script="$PWD/$script" ;;
    esac

    [ -f "$script" ] || {
      echo "Build script was not found: $script" >&2
      exit 1
    }

    # FFmpeg 8.1 removed the --disable-postproc configure option. Filter it from
    # the streamed script without modifying build.sh; $0 stays set to the original
    # path so build.sh still resolves its own directories correctly.
    echo "NixOS wrapper: omitting unsupported FFmpeg option --disable-postproc" >&2
    exec bash -c 'source_file=$1; shift; . "$source_file"' "$script" \
      <(sed '/--disable-postproc/d' "$script") "$@"
  '';
in
pkgs.buildFHSEnv {
  name = "renderingout-ffmpeg-builder";

  # The dev shell exports AS=as. x264 treats AS as an override for NASM, so
  # force the assembler back to nasm inside the clean FHS build environment.
  runScript = "env -u ASFLAGS AS=nasm ${runner}";

  targetPkgs = pkgs: with pkgs; [
    binutils
    cacert
    gcc
    git
    glibc.bin
    gnumake
    nasm
    pkg-config
  ];

  meta = {
    description = "FHS wrapper for building RenderingOut's Linux FFmpeg runtime";
  };
}
