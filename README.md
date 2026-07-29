# RenderingOut

RenderingOut is a Windows x64 native rendering plugin that encodes Unity
D3D11 textures to video files through FFmpeg.

## Features

- Encodes Unity `Texture` objects from their native D3D11 texture pointers.
- Tries H.264 encoders in order of availability, including NVENC, Media
  Foundation, AMF, QSV, and libx264.
- Uses direct D3D11 hardware frames where supported, including GPU-side
  RGBA/BGRA-to-NV12 conversion for Media Foundation, with a readback fallback.
- Accepts interleaved 32-bit float PCM audio and muxes it as AAC.
- Reports initialization and encoding errors through the Unity log.

> RenderingOut currently supports only Windows x64 projects using Unity's
> Direct3D 11 graphics API.

## Requirements

- Visual Studio 2022 with the **Desktop development with C++** workload
- CMake 3.21 or newer
- Ninja
- vcpkg
- A Unity installation containing the native `PluginAPI` headers

## Build

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

CMake automatically uses the newest Unity installation found under Unity Hub.
If Unity is installed elsewhere, provide the directory containing
`IUnityInterface.h` explicitly:

```powershell
cmake --preset x64-release `
  -DUNITY_PLUGIN_API="C:/path/to/Unity/Editor/Data/PluginAPI"
```

The vcpkg manifest installs the FFmpeg components required to build the plugin:
`avcodec`, `avformat`, `avutil`, `swscale`, and `swresample`.

## Unity integration

1. Copy `RenderingOut.dll` and its ABI-compatible FFmpeg DLLs into
   `Assets/Plugins/x86_64` in the Unity project.
2. Make sure the Unity project uses the Direct3D 11 graphics API.
3. Bind the exported functions declared in
   [`src/RenderingOut.h`](src/RenderingOut.h) from C#.
4. Create an encoder, submit texture frames, optionally register PCM audio,
   then free the encoder to flush and finalize the output file.

Important API constraints:

- Output width and height must be positive, even numbers.
- Submitted textures must match the configured dimensions and pixel format.
- Direct Media Foundation input requires a non-multisampled texture and a
  driver that supports D3D11 video processing plus encoder-bound NV12 surfaces.
- `video_encoder_submit_frame` must be called serially from the render thread.
- Register audio after the first video frame and before
  `video_encoder_free`. PCM samples must be normalized, interleaved 32-bit
  floats.
- Free all encoder instances before the Unity plugin is unloaded.

The available encoders depend on the FFmpeg runtime build. If the DLLs produced
by vcpkg do not include a suitable encoder, place DLLs from an ABI-compatible
full or custom shared FFmpeg build alongside `RenderingOut.dll`.

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE.txt).

## Development

This project was developed using vibe coding.
