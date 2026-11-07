cmake_minimum_required(VERSION 3.21)

project(RenderingOut LANGUAGES C)

if(NOT WIN32 AND NOT (UNIX AND NOT APPLE))
    message(FATAL_ERROR "RenderingOut supports Windows (D3D11) and Linux (OpenGL Core).")
endif()

# The Unity SDK is not a package dependency, so allow an explicit path while
# still making the common Unity Hub installation work out of the box.
set(UNITY_PLUGIN_API "$ENV{UNITY_PLUGIN_API}" CACHE PATH "Unity PluginAPI directory")
if(NOT UNITY_PLUGIN_API)
    if(WIN32)
        file(GLOB unity_plugin_api_candidates
            "C:/Program Files/Unity/Hub/Editor/*/Editor/Data/PluginAPI")
    else()
        file(GLOB unity_plugin_api_candidates
            "$ENV{HOME}/Unity/Hub/Editor/*/Editor/Data/PluginAPI"
            "/opt/Unity/Hub/Editor/*/Editor/Data/PluginAPI"
            "/opt/unity/Editor/Data/PluginAPI"
            "/opt/Unity/Editor/Data/PluginAPI")
    endif()
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

find_package(Threads REQUIRED)

# FFmpeg is a build-time SDK dependency only. Runtime library deployment
# belongs to the consuming Unity project and may come from a full/custom
# shared build.
if(WIN32)
    find_package(FFMPEG REQUIRED)
    set(RENDERINGOUT_FFMPEG_LIBRARIES ${FFMPEG_LIBRARIES})
    set(RENDERINGOUT_FFMPEG_INCLUDE_DIRS ${FFMPEG_INCLUDE_DIRS})
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
            libavcodec
            libavformat
            libavutil
            libswscale
            libswresample)
        set(RENDERINGOUT_FFMPEG_LIBRARIES PkgConfig::FFMPEG)
    else()
        find_package(FFMPEG REQUIRED)
        set(RENDERINGOUT_FFMPEG_LIBRARIES ${FFMPEG_LIBRARIES})
        set(RENDERINGOUT_FFMPEG_INCLUDE_DIRS ${FFMPEG_INCLUDE_DIRS})
    endif()
    find_package(OpenGL REQUIRED)
endif()

add_library(RenderingOut SHARED
    "src/RenderingOut.c"
    "src/RenderingOut.h"
    "src/Unity.c"
    "src/Unity.h")

if(WIN32)
    target_sources(RenderingOut PRIVATE
        "src/D3D11FrameConverter.c"
        "src/D3D11FrameConverter.h")
else()
    target_sources(RenderingOut PRIVATE
        "src/OpenGLFrameConverter.c"
        "src/OpenGLFrameConverter.h")
endif()

target_compile_features(RenderingOut PRIVATE c_std_11)
target_include_directories(RenderingOut PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${UNITY_PLUGIN_API}"
    ${RENDERINGOUT_FFMPEG_INCLUDE_DIRS})

if(WIN32)
    target_compile_definitions(RenderingOut PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX)
    target_link_libraries(RenderingOut PRIVATE
        ${RENDERINGOUT_FFMPEG_LIBRARIES}
        Threads::Threads
        d3d11
        dxgi
        ole32)
    if(MSVC)
        target_compile_options(RenderingOut PRIVATE /utf-8)
    endif()
else()
    target_compile_definitions(RenderingOut PRIVATE _FILE_OFFSET_BITS=64)
    target_link_libraries(RenderingOut PRIVATE
        ${RENDERINGOUT_FFMPEG_LIBRARIES}
        Threads::Threads
        OpenGL::GL
        m
        ${CMAKE_DL_LIBS})
endif()
