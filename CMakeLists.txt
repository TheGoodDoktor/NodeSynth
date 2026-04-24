cmake_minimum_required(VERSION 3.24)
project(NodeSynth LANGUAGES C CXX)

# Allow the older FetchContent_Populate form for dependencies whose upstream
# CMakeLists we don't want to invoke (imgui-node-editor below).
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

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
# Enable ImVec2/ImVec4 math operators project-wide via IMGUI_USER_CONFIG.
# This replaces ImGui's default imconfig.h with our own, which defines
# IMGUI_DEFINE_MATH_OPERATORS. imgui-node-editor requires this macro be in
# effect at the first include of <imgui.h>, so we set it centrally rather
# than relying on include-order in every consumer.
target_compile_definitions(imgui PUBLIC
    IMGUI_USER_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/src/NodeSynthImConfig.h"
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

# ---- imgui-node-editor ------------------------------------------------------
# Upstream ships a CMakeLists that expects ImGui via find_package — not how we
# vendor it. Populate the sources only and build a target of our own.
FetchContent_Declare(imgui_node_editor
    GIT_REPOSITORY https://github.com/thedmd/imgui-node-editor.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(imgui_node_editor)
if(NOT imgui_node_editor_POPULATED)
    FetchContent_Populate(imgui_node_editor)
    # Patch: imgui-node-editor's imgui_extra_math.inl unconditionally defines
    # operator*(float, ImVec2&), which recent Dear ImGui also provides (under
    # IMGUI_DEFINE_MATH_OPERATORS). Drop node-editor's copy; ImGui's remains.
    set(NE_INL "${imgui_node_editor_SOURCE_DIR}/imgui_extra_math.inl")
    file(READ "${NE_INL}" NE_INL_CONTENT)
    string(REGEX REPLACE
        "inline ImVec2 operator\\*\\(const float lhs, const ImVec2& rhs\\)[^}]*\\}"
        "// patched out by NodeSynth: Dear ImGui defines this under IMGUI_DEFINE_MATH_OPERATORS"
        NE_INL_CONTENT "${NE_INL_CONTENT}")
    file(WRITE "${NE_INL}" "${NE_INL_CONTENT}")
endif()

add_library(imgui_node_editor_lib STATIC
    ${imgui_node_editor_SOURCE_DIR}/imgui_node_editor.cpp
    ${imgui_node_editor_SOURCE_DIR}/imgui_node_editor_api.cpp
    ${imgui_node_editor_SOURCE_DIR}/imgui_canvas.cpp
    ${imgui_node_editor_SOURCE_DIR}/crude_json.cpp
)
target_include_directories(imgui_node_editor_lib PUBLIC ${imgui_node_editor_SOURCE_DIR})
target_link_libraries(imgui_node_editor_lib PUBLIC imgui)

# ---- NodeSynth executable ---------------------------------------------------
# Console subsystem on Windows for now (keeps stdout/stderr visible during dev).
# We'll switch to the GUI subsystem + WinMain shim when the project is user-facing.
add_executable(nodesynth MACOSX_BUNDLE
    src/main.cpp
    src/graph/Graph.cpp
    src/ui/Editor.cpp
)
target_include_directories(nodesynth PRIVATE src)
target_compile_definitions(nodesynth PRIVATE NOMINMAX)
target_link_libraries(nodesynth PRIVATE imgui miniaudio imgui_node_editor_lib)

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
