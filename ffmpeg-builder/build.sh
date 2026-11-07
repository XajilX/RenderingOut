#!/usr/bin/env sh
#
# Builds the minimal linux-x64 FFmpeg shared runtime required by RenderingOut.
#
# The build keeps only the FFmpeg features used by the Linux OpenGL renderer:
#
#   Video encoders : h264_nvenc, libx264
#   Audio encoder  : aac
#   Container      : MOV/MP4 muxer
#   I/O            : file output protocol
#   Conversion     : libswscale and libswresample
#
# All decoders, demuxers, filters, devices, network protocols, documentation and
# FFmpeg command-line programs are disabled. x264 is linked statically into the
# FFmpeg shared libraries so the runtime package contains only FFmpeg libraries.
#
# Usage:
#   sh build.sh [--jobs N] [--ffmpeg-ref REF]
#
# The finished runtime is written to ffmpeg-builder/dist/linux-x64.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
BUILDER_ROOT=$SCRIPT_DIR
WORK_ROOT=$BUILDER_ROOT/.work
SOURCE_ROOT=$WORK_ROOT/src
BUILD_ROOT=$WORK_ROOT/build
INSTALL_ROOT=$WORK_ROOT/install
STAGING_ROOT=$WORK_ROOT/package
OUTPUT_ROOT=$BUILDER_ROOT/dist/linux-x64
FFMPEG_RELEASE_BRANCH=${FFBUILD_FFMPEG_REF:-release/8.1}

JOBS=${FFBUILD_JOBS:-}
if [ -z "$JOBS" ]; then
    JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || printf '1')
fi

usage() {
    cat <<'EOF'
Usage: sh build.sh [options]

Options:
  -j, --jobs N          Parallel build jobs (default: host CPU count, max 256)
      --ffmpeg-ref REF  FFmpeg Git branch or tag (default: release/8.1)
  -h, --help            Show this help

Required tools:
  git, make, gcc, g++, ar, nasm, pkg-config, readelf, ldd, strip

On Debian/Ubuntu, install them with:
  sudo apt install build-essential git nasm pkg-config binutils
EOF
}

die() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

write_step() {
    printf '\n==> %s\n' "$*"
}

run_checked() {
    "$@" || {
        command_status=$?
        printf 'Command failed with exit code %s: %s\n' "$command_status" "$*" >&2
        exit "$command_status"
    }
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        -j|--jobs)
            [ "$#" -ge 2 ] || die "Missing value for $1"
            JOBS=$2
            shift 2
            ;;
        --jobs=*)
            JOBS=${1#*=}
            shift
            ;;
        --ffmpeg-ref)
            [ "$#" -ge 2 ] || die "Missing value for $1"
            FFMPEG_RELEASE_BRANCH=$2
            shift 2
            ;;
        --ffmpeg-ref=*)
            FFMPEG_RELEASE_BRANCH=${1#*=}
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        *)
            die "Unknown option: $1"
            ;;
    esac
done

[ "$#" -eq 0 ] || die "Unexpected arguments: $*"
case "$JOBS" in
    ''|*[!0-9]*) die "Jobs must be an integer from 1 to 256: $JOBS" ;;
esac
[ "$JOBS" -ge 1 ] && [ "$JOBS" -le 256 ] ||
    die "Jobs must be an integer from 1 to 256: $JOBS"
[ -n "$FFMPEG_RELEASE_BRANCH" ] || die "The FFmpeg ref cannot be empty."

host_os=$(uname -s 2>/dev/null || printf 'unknown')
[ "$host_os" = Linux ] ||
    die "This script builds only linux-x64 and must run on Linux."
host_arch=$(uname -m 2>/dev/null || printf 'unknown')
case "$host_arch" in
    x86_64|amd64) ;;
    *) die "A 64-bit x86 Linux host is required; detected '$host_arch'." ;;
esac

missing_tools=
for tool in git make gcc g++ ar nasm pkg-config readelf ldd strip; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        missing_tools="$missing_tools $tool"
    fi
done
if [ -n "$missing_tools" ]; then
    cat >&2 <<EOF
Missing required build tools:$missing_tools

On Debian/Ubuntu, install them with:
  sudo apt install build-essential git nasm pkg-config binutils
EOF
    exit 1
fi

source_url() {
    case "$1" in
        FFmpeg) printf '%s\n' "https://github.com/FFmpeg/FFmpeg.git" ;;
        x264) printf '%s\n' "https://code.videolan.org/videolan/x264.git" ;;
        NvCodecHeaders) printf '%s\n' "https://github.com/FFmpeg/nv-codec-headers.git" ;;
        *) die "Unknown source: $1" ;;
    esac
}

source_ref() {
    case "$1" in
        FFmpeg) printf '%s\n' "$FFMPEG_RELEASE_BRANCH" ;;
        x264|NvCodecHeaders) printf '%s\n' "master" ;;
        *) die "Unknown source: $1" ;;
    esac
}

source_path() {
    case "$1" in
        FFmpeg) printf '%s\n' "$SOURCE_ROOT/ffmpeg" ;;
        x264) printf '%s\n' "$SOURCE_ROOT/x264" ;;
        NvCodecHeaders) printf '%s\n' "$SOURCE_ROOT/nv-codec-headers" ;;
        *) die "Unknown source: $1" ;;
    esac
}

assert_path_inside() {
    checked_path=$1
    allowed_root=$2
    case "$checked_path" in
        "$allowed_root"/*) ;;
        *)
            die "Refusing to modify path outside the intended root '$allowed_root': '$checked_path'"
            ;;
    esac
}

reset_owned_directory() {
    owned_path=$1
    allowed_root=$2
    assert_path_inside "$owned_path" "$allowed_root"
    if [ -e "$owned_path" ]; then
        rm -rf -- "$owned_path"
    fi
    mkdir -p -- "$owned_path"
}

sync_repository() {
    repo_name=$1
    repo_url=$2
    repo_ref=$3
    repo_path=$4

    assert_path_inside "$repo_path" "$SOURCE_ROOT"

    if [ -d "$repo_path/.git" ]; then
        actual_remote=$(git -C "$repo_path" remote get-url origin) ||
            die "Unable to read the origin URL for $repo_name at '$repo_path'."
        actual_remote_lower=$(printf '%s' "$actual_remote" | tr '[:upper:]' '[:lower:]')
        expected_remote_lower=$(printf '%s' "$repo_url" | tr '[:upper:]' '[:lower:]')
        [ "$actual_remote_lower" = "$expected_remote_lower" ] ||
            die "$repo_name source directory has an unexpected origin. Expected '$repo_url', found '$actual_remote'."

        write_step "Updating $repo_name ($repo_ref)"
        run_checked git -C "$repo_path" fetch --prune --no-tags --depth=1 origin "$repo_ref"
        run_checked git -C "$repo_path" checkout --detach --force FETCH_HEAD
        run_checked git -C "$repo_path" clean -ffd
    elif [ -e "$repo_path" ]; then
        die "Source path exists but is not the expected Git checkout: '$repo_path'"
    else
        write_step "Cloning $repo_name ($repo_ref)"
        run_checked git clone \
            --depth=1 \
            --no-tags \
            --single-branch \
            --branch "$repo_ref" \
            "$repo_url" \
            "$repo_path"
    fi
}

mkdir -p -- "$SOURCE_ROOT"
for source_name in FFmpeg x264 NvCodecHeaders; do
    sync_repository \
        "$source_name" \
        "$(source_url "$source_name")" \
        "$(source_ref "$source_name")" \
        "$(source_path "$source_name")"
done

reset_owned_directory "$BUILD_ROOT" "$WORK_ROOT"
reset_owned_directory "$INSTALL_ROOT" "$WORK_ROOT"
reset_owned_directory "$STAGING_ROOT" "$WORK_ROOT"

FFMPEG_SOURCE=$(source_path FFmpeg)
X264_SOURCE=$(source_path x264)
NV_SOURCE=$(source_path NvCodecHeaders)

DEPS_PREFIX=$INSTALL_ROOT/deps
FFMPEG_PREFIX=$INSTALL_ROOT/ffmpeg
FFMPEG_BUILD=$BUILD_ROOT/ffmpeg
PROBE_BUILD=$BUILD_ROOT/probe

mkdir -p -- "$DEPS_PREFIX" "$FFMPEG_PREFIX" "$FFMPEG_BUILD" "$PROBE_BUILD"

write_step "Installing current NVIDIA codec headers"
run_checked make -C "$NV_SOURCE" PREFIX="$DEPS_PREFIX" install

write_step "Building minimal 8-bit 4:2:0 x264 static library"
cd "$X264_SOURCE"
make distclean >/dev/null 2>&1 || true
run_checked ./configure \
    --prefix="$DEPS_PREFIX" \
    --enable-static \
    --disable-shared \
    --disable-cli \
    --enable-pic \
    --bit-depth=8 \
    --chroma-format=420 \
    --disable-opencl \
    --extra-cflags="-Os -ffunction-sections -fdata-sections"
run_checked make -j"$JOBS"
run_checked make install

export PKG_CONFIG_PATH="$DEPS_PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$DEPS_PREFIX/lib/pkgconfig"
export PKG_CONFIG_SYSTEM_INCLUDE_PATH=
export PKG_CONFIG_SYSTEM_LIBRARY_PATH=

for package in x264 ffnvcodec; do
    pkg-config --exists "$package" ||
        die "Dependency '$package' was built but is not visible to pkg-config."
done

write_step "Configuring minimal shared FFmpeg libraries"
cd "$FFMPEG_BUILD"
run_checked "$FFMPEG_SOURCE/configure" \
    --prefix="$FFMPEG_PREFIX" \
    --target-os=linux \
    --arch=x86_64 \
    --enable-shared \
    --disable-static \
    --enable-pic \
    --enable-small \
    --enable-gpl \
    --disable-programs \
    --disable-doc \
    --disable-debug \
    --disable-network \
    --disable-autodetect \
    --disable-avdevice \
    --disable-avfilter \
    --disable-postproc \
    --enable-pthreads \
    --disable-everything \
    --enable-avcodec \
    --enable-avformat \
    --enable-avutil \
    --enable-swscale \
    --enable-swresample \
    --enable-encoder=aac \
    --enable-encoder=h264_nvenc \
    --enable-encoder=libx264 \
    --enable-muxer=mov,mp4 \
    --enable-protocol=file \
    --enable-libx264 \
    --enable-ffnvcodec \
    --enable-nvenc \
    --disable-nvdec \
    --disable-cuvid \
    --disable-vaapi \
    --disable-vdpau \
    --disable-vulkan \
    --disable-iconv \
    --disable-zlib \
    --disable-bzlib \
    --disable-lzma \
    --disable-sdl2 \
    --disable-swscale-alpha \
    --pkg-config-flags=--static \
    --extra-cflags="-I$DEPS_PREFIX/include -ffunction-sections -fdata-sections" \
    --extra-ldflags="-L$DEPS_PREFIX/lib -Wl,--gc-sections"

for item in \
    CONFIG_AAC_ENCODER \
    CONFIG_H264_NVENC_ENCODER \
    CONFIG_LIBX264_ENCODER \
    CONFIG_MOV_MUXER \
    CONFIG_MP4_MUXER \
    CONFIG_FILE_PROTOCOL
do
    grep -Eq "^#define ${item} 1$" config_components.h ||
        die "FFmpeg configure did not enable required component: $item"
done

for forbidden in CONFIG_AVFILTER CONFIG_AVDEVICE; do
    if grep -Eq "^#define ${forbidden} 1$" config.h; then
        die "FFmpeg configure unexpectedly enabled forbidden library: $forbidden"
    fi
done

write_step "Building and installing FFmpeg shared libraries"
run_checked make -j"$JOBS"
run_checked make install

cat > "$PROBE_BUILD/probe.c" <<'PROBE_EOF'
#include <stdio.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

static int require_encoder(const char *name)
{
    if (avcodec_find_encoder_by_name(name))
        return 0;
    fprintf(stderr, "missing encoder: %s\n", name);
    return 1;
}

int main(void)
{
    const char *encoders[] = { "h264_nvenc", "libx264", "aac" };
    void *protocol_state = NULL;
    const char *protocol;
    int found_file_protocol = 0;
    int failed = 0;
    unsigned int i;

    for (i = 0; i < sizeof(encoders) / sizeof(encoders[0]); ++i)
        failed |= require_encoder(encoders[i]);

    if (!av_guess_format("mp4", NULL, NULL)) {
        fprintf(stderr, "missing MP4/MOV muxer\n");
        failed = 1;
    }

    while ((protocol = avio_enum_protocols(&protocol_state, 1)) != NULL) {
        if (strcmp(protocol, "file") == 0)
            found_file_protocol = 1;
    }
    if (!found_file_protocol) {
        fprintf(stderr, "missing file output protocol\n");
        failed = 1;
    }

    if (!sws_isSupportedInput(AV_PIX_FMT_RGBA) ||
        !sws_isSupportedInput(AV_PIX_FMT_BGRA) ||
        !sws_isSupportedOutput(AV_PIX_FMT_YUV420P) ||
        !sws_isSupportedOutput(AV_PIX_FMT_NV12)) {
        fprintf(stderr, "missing required libswscale pixel-format support\n");
        failed = 1;
    }

    {
        SwrContext *swr = swr_alloc();
        if (!swr) {
            fprintf(stderr, "libswresample allocation failed\n");
            failed = 1;
        }
        swr_free(&swr);
    }

    if (!failed)
        puts("RenderingOut Linux FFmpeg capability probe passed.");
    return failed;
}
PROBE_EOF

write_step "Verifying the installed libraries through the public FFmpeg APIs"
run_checked gcc \
    -Os \
    -I"$FFMPEG_PREFIX/include" \
    "$PROBE_BUILD/probe.c" \
    -L"$FFMPEG_PREFIX/lib" \
    -lavformat -lavcodec -lswscale -lswresample -lavutil \
    -o "$PROBE_BUILD/probe"
LD_LIBRARY_PATH="$FFMPEG_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$PROBE_BUILD/probe"

write_step "Packaging FFmpeg shared libraries"
packaged_libraries=$PROBE_BUILD/packaged-libraries.txt
: > "$packaged_libraries"

copy_ffmpeg_library() {
    library_base=$1
    actual_library=$(find "$FFMPEG_PREFIX/lib" -maxdepth 1 -type f \
        -name "lib${library_base}.so.*" -print | sort | tail -n 1)
    [ -n "$actual_library" ] ||
        die "Unable to find the installed FFmpeg library: lib${library_base}.so.*"

    dynamic_section=$(LC_ALL=C readelf -d "$actual_library") ||
        die "Unable to read the dynamic section for '$actual_library'."
    soname=$(printf '%s\n' "$dynamic_section" |
        sed -n 's/.*(SONAME).*\[\(.*\)\].*/\1/p' |
        head -n 1)
    [ -n "$soname" ] ||
        die "Unable to determine the SONAME for '$actual_library'."

    cp -L "$actual_library" "$STAGING_ROOT/$soname"
    printf '%s\n' "$soname" >> "$packaged_libraries"
}

for library_base in avcodec avformat avutil swscale swresample; do
    copy_ffmpeg_library "$library_base"
done

if LC_ALL=C readelf -d "$STAGING_ROOT"/*.so.* 2>/dev/null |
    grep -Eq '\(NEEDED\).*\[libx264'; then
    die "libx264 was linked dynamically; the runtime package must contain only FFmpeg libraries."
fi

for staged_library in "$STAGING_ROOT"/*.so.*; do
    [ -f "$staged_library" ] || continue
    if ! dependency_report=$(LC_ALL=C \
        LD_LIBRARY_PATH="$STAGING_ROOT${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        ldd "$staged_library" 2>&1); then
        die "Unable to inspect runtime dependencies for '$staged_library': $dependency_report"
    fi
    missing_dependencies=$(printf '%s\n' "$dependency_report" |
        sed -n 's/.*=> not found.*/&/p')
    [ -z "$missing_dependencies" ] ||
        die "Unresolved runtime dependencies for '$staged_library':$missing_dependencies"
done

cp "$FFMPEG_BUILD/config.h" "$STAGING_ROOT/FFMPEG_CONFIG.h"
cp "$FFMPEG_BUILD/config_components.h" "$STAGING_ROOT/FFMPEG_COMPONENTS.h"
cp "$FFMPEG_SOURCE/COPYING.GPLv2" "$STAGING_ROOT/LICENSE.FFmpeg-GPLv2.txt"
cp "$X264_SOURCE/COPYING" "$STAGING_ROOT/LICENSE.x264.txt"

if [ -f "$NV_SOURCE/LICENSE" ]; then
    cp "$NV_SOURCE/LICENSE" "$STAGING_ROOT/LICENSE.ffnvcodec.txt"
elif [ -f "$NV_SOURCE/LICENSE.txt" ]; then
    cp "$NV_SOURCE/LICENSE.txt" "$STAGING_ROOT/LICENSE.ffnvcodec.txt"
fi

printf '\nPackaged runtime libraries:\n'
while IFS= read -r library_name; do
    printf '  %s\n' "$library_name"
done < "$packaged_libraries"

{
    printf 'Target: linux-x64\n'
    printf 'Built at (UTC): %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    printf 'FFmpeg branch or tag: %s\n' "$FFMPEG_RELEASE_BRANCH"
    printf 'License: GPL v2 or later (FFmpeg is built with GPL-licensed x264)\n'
    printf '\n'
    printf 'Enabled encoders: h264_nvenc, libx264, aac\n'
    printf 'Enabled muxers: mov, mp4\n'
    printf 'Enabled protocol: file\n'
    printf 'Enabled libraries: avcodec, avformat, avutil, swscale, swresample\n'
    printf '\n'
    printf 'Deployment: copy every packaged .so file beside libRenderingOut.so and launch Unity with an rpath or LD_LIBRARY_PATH that includes that directory.\n'
    printf 'Unity graphics API: OpenGL Core (launch with -force-glcore).\n'
    printf '\n'
    printf 'Source revisions:\n'
    for source_name in FFmpeg x264 NvCodecHeaders; do
        commit=$(git -C "$(source_path "$source_name")" rev-parse HEAD) ||
            die "Unable to record the source revision for $source_name."
        printf '  %s: %s (%s)\n' \
            "$source_name" \
            "$commit" \
            "$(source_ref "$source_name")"
    done
} > "$STAGING_ROOT/BUILD_INFO.txt"

runtime_library_count=$(wc -l < "$packaged_libraries" | tr -d '[:space:]')
[ "$runtime_library_count" -ge 5 ] ||
    die "The build completed but fewer than the five required FFmpeg libraries were packaged."

reset_owned_directory "$OUTPUT_ROOT" "$BUILDER_ROOT"
cp -a "$STAGING_ROOT/." "$OUTPUT_ROOT/"

printf '\nBuild complete: %s\n' "$OUTPUT_ROOT"
printf 'Deploy every .so file below beside libRenderingOut.so.\n'
printf 'Runtime libraries:\n'
find "$OUTPUT_ROOT" -maxdepth 1 -type f -name '*.so.*' -printf '%f\n' | sort |
    while IFS= read -r library_name; do
        library_size=$(wc -c < "$OUTPUT_ROOT/$library_name" | tr -d '[:space:]')
        printf '  %-28s %10s bytes\n' "$library_name" "$library_size"
    done
