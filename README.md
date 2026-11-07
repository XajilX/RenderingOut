# RenderingOut

RenderingOut is a native Unity rendering plugin that encodes textures to video
files through FFmpeg. It supports Windows with Direct3D 11 and Linux with the
OpenGL Core renderer.

## Features

- Encodes Unity `Texture` objects from their native texture pointers.
- Uses the D3D11 hardware path on Windows, including direct hardware frames
  where supported and GPU-side RGBA/BGRA-to-NV12 conversion for Media
  Foundation.
- Reads back OpenGL textures and encodes them with `h264_nvenc` or libx264 on
  Linux.
- Accepts interleaved 32-bit float PCM audio and muxes it as AAC.
- Reports initialization and encoding errors through the Unity log.

## Platform support

| Platform | Unity graphics API | Texture input | Hardware direct path |
| --- | --- | --- | --- |
| Windows x64 | Direct3D 11 | `ID3D11Texture2D` | Yes, when supported |
| Linux x64 | OpenGL Core | `GL_TEXTURE_2D` RGBA8 | No, CPU readback |

Vulkan textures are not supported. On Linux, launch Unity with
`-force-glcore` so that `Texture.GetNativeTexturePtr()` returns an OpenGL
texture name.

## Requirements

Common requirements:

- CMake 3.21 or newer
- Ninja
- A Unity installation containing the native `PluginAPI` headers
- An FFmpeg runtime that provides the `avcodec`, `avformat`, `avutil`,
  `swscale`, and `swresample` libraries plus the `libx264` encoder

Windows requirements:

- Visual Studio 2022 with the **Desktop development with C++** workload
- vcpkg

Linux requirements:

- A C11 compiler and POSIX threads
- OpenGL development headers
- FFmpeg development packages, for example on Debian/Ubuntu:

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
  libswresample-dev libgl1-mesa-dev
```

## Build

### Windows

Run the following commands from a Visual Studio Developer PowerShell:

```powershell
$env:VCPKG_ROOT = "C:/path/to/vcpkg"

cmake --preset x64-release
cmake --build --preset x64-release
```

The release output is written to:

```text
out/build/x64-release/RenderingOut.dll
```

For a debug build, replace `x64-release` with `x64-debug`.

### Linux

Run from a shell:

```bash
cmake --preset linux-release
cmake --build --preset linux-release
```

The release output is written to:

```text
out/build/linux-release/libRenderingOut.so
```

For a debug build, replace `linux-release` with `linux-debug`.

### Nix

The repository includes a `flake.nix` with the Linux build dependencies. Enter
the development shell, point it at Unity's PluginAPI directory, and build:

```bash
nix develop
export UNITY_PLUGIN_API="/path/to/Unity/Editor/Data/PluginAPI"
cmake --preset linux-release
cmake --build --preset linux-release
```

CMake automatically uses the newest Unity installation found under Unity Hub,
including `$HOME/Unity/Hub/Editor` on Linux. If Unity is installed elsewhere,
provide the directory containing `IUnityInterface.h` explicitly:

```bash
cmake --preset linux-release \
  -DUNITY_PLUGIN_API="/path/to/Unity/Editor/Data/PluginAPI"
```

On Windows, CMake uses the vcpkg manifest to install the required FFmpeg
components. On Linux, CMake uses `pkg-config` and the system FFmpeg packages.

## Unity integration

1. Copy the native plugin into `Assets/Plugins/x86_64` in the Unity project.
   Use `RenderingOut.dll` on Windows or `libRenderingOut.so` on Linux.
2. On Linux, start Unity with `-force-glcore` and use the OpenGL Core graphics
   API. On Windows, use the Direct3D 11 graphics API.
3. Bind the exported functions declared in
   [`src/RenderingOut.h`](src/RenderingOut.h) from C#.
4. Create an encoder, submit texture frames, optionally register PCM audio,
   then free the encoder to flush and finalize the output file.

On Windows, copy the ABI-compatible FFmpeg DLLs next to `RenderingOut.dll` if
they are not already available to the Unity process. On Linux, install the
FFmpeg shared libraries system-wide or make them available through the
process library search path.

Important API constraints:

- Output width and height must be positive, even numbers.
- Submitted textures must match the configured dimensions and pixel format.
- `video_encoder_submit_frame` must be called serially from the render thread
  that owns the texture.
- Linux input textures must be non-multisampled `GL_TEXTURE_2D` textures with
  8-bit RGB(A) color channels. Typical Unity `Texture2D` and `RenderTexture`
  RGBA8 formats meet this requirement.
- Register audio after the first video frame and before
  `video_encoder_free`. PCM samples must be normalized, interleaved 32-bit
  floats.
- Free all encoder instances before the Unity plugin is unloaded.
- The Windows direct Media Foundation path requires a non-multisampled texture
  and a driver that supports D3D11 video processing plus encoder-bound NV12
  surfaces.

The available encoders depend on the FFmpeg runtime build. Linux tries
`h264_nvenc` first and falls back to libx264. Windows tries the available
hardware encoders before falling back to libx264. If the FFmpeg libraries do
not include a suitable encoder, install an ABI-compatible full or custom
shared FFmpeg build.

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE.txt).

## Development

This project was developed using vibe coding.
