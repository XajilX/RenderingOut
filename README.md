# RenderingOut

Windows x64 Unity native rendering plugin backed by FFmpeg.

## Build

Requirements:

- Visual Studio with the MSVC C/C++ workload
- CMake 3.21 or newer
- vcpkg

The supplied Windows presets use the standard vcpkg CMake toolchain. Make
`VCPKG_ROOT` point to vcpkg before configuring:

```powershell
$env:VCPKG_ROOT = "C:/path/to/vcpkg"
```

Configure and build:

```powershell
cmake --preset x64-release
cmake --build --preset x64-release
```

The resulting plugin is under `out/build/msvc-x64-release`.

The vcpkg manifest installs only the FFmpeg libraries required at build time:
`avcodec`, `avformat`, `avutil`, and `swscale`.

Deploy ABI-compatible shared FFmpeg runtime DLLs from a full distribution or
from a separate custom FFmpeg build alongside `RenderingOut.dll`.
