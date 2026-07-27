# CMakeList.txt: RenderingOut 的 CMake 项目，在此处包括源代码并定义
# 项目特定的逻辑。
#

find_package(FFMPEG REQUIRED)

add_library(RenderingOut SHARED
    "RenderingOut.c"
    "RenderingOut.h"
    "Unity.c"
    "Unity.h")

target_include_directories(RenderingOut PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FFMPEG_INCLUDE_DIRS}
)
target_include_directories(RenderingOut PRIVATE
    "C:/Program Files/Unity/Hub/Editor/6000.3.19f1/Editor/Data/PluginAPI"
)

target_link_libraries(RenderingOut PRIVATE
    ${FFMPEG_LIBRARIES}
)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET RenderingOut PROPERTY C_STANDARD 11)
endif()

# MSVC: 源文件含中文，指定 UTF-8 编码避免 C4819
if (MSVC)
  target_compile_options(RenderingOut PRIVATE /utf-8)
endif()

# TODO: 如有需要，请添加测试并安装目标。
