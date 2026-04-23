cmake_minimum_required(VERSION 3.24)
project(NodeSynth LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(FetchContent)

# ---- GLFW -------------------------------------------------------------------
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glfw)

# ---- Dear ImGui (docking branch) -------------------------------------------
# No upstream CMakeLists, so we build it ourselves.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw)

# ---- miniaudio --------------------------------------------------------------
# Upstream ships its own CMakeLists that exposes a `miniaudio` target —
# we link it directly rather than building our own.
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(miniaudio)

# ---- imgui-node-editor: deferred until Phase 1. ----------------------------

# ---- NodeSynth executable ---------------------------------------------------
# Console subsystem on Windows for now (keeps stdout/stderr visible during dev).
# We'll switch to the GUI subsystem + WinMain shim when the project is user-facing.
add_executable(nodesynth MACOSX_BUNDLE
    src/main.cpp
)
target_link_libraries(nodesynth PRIVATE imgui miniaudio)

if(WIN32)
    target_link_libraries(nodesynth PRIVATE opengl32)
elseif(APPLE)
    target_link_libraries(nodesynth PRIVATE "-framework OpenGL")
    set_target_properties(nodesynth PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME        "NodeSynth"
        MACOSX_BUNDLE_GUI_IDENTIFIER     "co.lucidgames.nodesynth"
        MACOSX_BUNDLE_BUNDLE_VERSION     "0.1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
    )
endif()

if(MSVC)
    target_compile_options(nodesynth PRIVATE /W4 /permissive-)
else()
    target_compile_options(nodesynth PRIVATE -Wall -Wextra -Wpedantic)
endif()
