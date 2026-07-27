cmake_minimum_required(VERSION 3.21)

project(RenderingOut LANGUAGES C)

if(NOT WIN32)
    message(FATAL_ERROR "RenderingOut currently requires Unity's D3D11 renderer.")
endif()

# The Unity SDK is not a package dependency, so allow an explicit path while
# still making the common Unity Hub installation work out of the box.
set(UNITY_PLUGIN_API "" CACHE PATH "Unity PluginAPI directory")
if(NOT UNITY_PLUGIN_API)
    file(GLOB unity_plugin_api_candidates
        "C:/Program Files/Unity/Hub/Editor/*/Editor/Data/PluginAPI")
    list(SORT unity_plugin_api_candidates COMPARE NATURAL ORDER DESCENDING)
    if(unity_plugin_api_candidates)
        list(GET unity_plugin_api_candidates 0 UNITY_PLUGIN_API)
    endif()
endif()

if(NOT EXISTS "${UNITY_PLUGIN_API}/IUnityInterface.h")
    message(FATAL_ERROR
        "Unity PluginAPI was not found. Set UNITY_PLUGIN_API to "
        "<Unity editor>/Editor/Data/PluginAPI.")
endif()

# FFmpeg is a build-time SDK dependency only. Runtime DLL deployment belongs
# to the consuming Unity project and may come from a full/custom shared build.
find_package(FFMPEG REQUIRED)

add_library(RenderingOut SHARED
    "src/D3D11FrameConverter.c"
    "src/D3D11FrameConverter.h"
    "src/RenderingOut.c"
    "src/RenderingOut.h"
    "src/Unity.c"
    "src/Unity.h")

target_compile_features(RenderingOut PRIVATE c_std_11)
target_compile_definitions(RenderingOut PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX)
target_include_directories(RenderingOut PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${UNITY_PLUGIN_API}"
    ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(RenderingOut PRIVATE
    ${FFMPEG_LIBRARIES}
    d3d11
    dxgi
    ole32)

if(MSVC)
    target_compile_options(RenderingOut PRIVATE /utf-8)
endif()
