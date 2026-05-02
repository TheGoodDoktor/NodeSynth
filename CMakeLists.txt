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

# ---- RtMidi -----------------------------------------------------------------
# Cross-platform MIDI: WinMM on Windows, CoreMIDI on macOS, ALSA on Linux.
set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTMIDI_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(RTMIDI_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(rtmidi
    GIT_REPOSITORY https://github.com/thestk/rtmidi.git
    GIT_TAG        6.0.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(rtmidi)

# ---- nlohmann/json ----------------------------------------------------------
# Header-only JSON for save/load. JSON_BuildTests off keeps configure quick.
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ---- nativefiledialog-extended ---------------------------------------------
# OS-native Open / Save dialogs. Falls back to a typed-path popup at runtime
# if the dialog API returns an error (sandboxed environments etc.).
FetchContent_Declare(nfd
    GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
    GIT_TAG        v1.2.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nfd)

# ---- floooh/chips (Zlib): header-only 8-bit chip emulators -----------------
# Used by FSidPlayer for m6502 + m6526 (CIA) + m6569 (VIC-II) + m6581 (SID).
# Pure header-only; no CMake build, just populate and add the chips/ folder
# to include paths.
FetchContent_Declare(chips
    GIT_REPOSITORY https://github.com/floooh/chips.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(chips)
if(NOT chips_POPULATED)
    FetchContent_Populate(chips)
endif()

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

# Glob so new nodes/panels are picked up without editing this file. CONFIGURE_DEPENDS
# makes the build system re-scan on rebuild; the tradeoff vs. an explicit list is
# acceptable at this project's size.
file(GLOB_RECURSE NODESYNTH_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

add_executable(nodesynth MACOSX_BUNDLE ${NODESYNTH_SOURCES})
target_include_directories(nodesynth PRIVATE src "${chips_SOURCE_DIR}/chips")
target_compile_definitions(nodesynth PRIVATE NOMINMAX)
target_link_libraries(nodesynth PRIVATE imgui miniaudio imgui_node_editor_lib rtmidi nlohmann_json::nlohmann_json nfd)

# Mirror the on-disk src/ tree into Visual Studio filter folders (and Xcode groups).
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "src" FILES ${NODESYNTH_SOURCES})

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
    # C4324: "structure was padded due to alignment specifier" — expected, we use alignas() intentionally on audio buffers.
    target_compile_options(nodesynth PRIVATE /W4 /permissive- /wd4324)
else()
    target_compile_options(nodesynth PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---- Bundled presets --------------------------------------------------------
# Mirror the source-tree presets/ folder into the build output so the running
# exe finds them next to itself. macOS bundles use the Resources directory
# convention; everything else drops them next to the binary.
set(NODESYNTH_PRESET_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/presets")
if(EXISTS "${NODESYNTH_PRESET_SOURCE_DIR}")
    if(APPLE)
        # Copy each preset .json into the .app bundle's Resources/presets/.
        file(GLOB_RECURSE NODESYNTH_PRESET_FILES CONFIGURE_DEPENDS
            "${NODESYNTH_PRESET_SOURCE_DIR}/*.json")
        foreach(PRESET_FILE IN LISTS NODESYNTH_PRESET_FILES)
            file(RELATIVE_PATH PRESET_REL "${NODESYNTH_PRESET_SOURCE_DIR}" "${PRESET_FILE}")
            get_filename_component(PRESET_REL_DIR "${PRESET_REL}" DIRECTORY)
            target_sources(nodesynth PRIVATE "${PRESET_FILE}")
            set_source_files_properties("${PRESET_FILE}" PROPERTIES
                MACOSX_PACKAGE_LOCATION "Resources/presets/${PRESET_REL_DIR}")
        endforeach()
    else()
        add_custom_command(TARGET nodesynth POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${NODESYNTH_PRESET_SOURCE_DIR}"
                "$<TARGET_FILE_DIR:nodesynth>/presets"
            COMMENT "Copying bundled presets next to nodesynth binary")
    endif()
endif()

# ---- Tests (Catch2 v3) ------------------------------------------------------
option(NODESYNTH_BUILD_TESTS "Build NodeSynth unit tests" ON)
if(NODESYNTH_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.4
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)

    file(GLOB_RECURSE NODESYNTH_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
    )

    add_executable(nodesynth_tests ${NODESYNTH_TEST_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/graph/Graph.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/graph/EditHistory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/io/PatchSerializer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/NodeRegistry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/MidiInput.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/sid/SidEmulator.cpp
    )
    target_include_directories(nodesynth_tests PRIVATE src "${chips_SOURCE_DIR}/chips")
    target_compile_definitions(nodesynth_tests PRIVATE NOMINMAX)
    target_link_libraries(nodesynth_tests PRIVATE Catch2::Catch2WithMain nlohmann_json::nlohmann_json rtmidi)

    enable_testing()
    include(CTest)
    include(Catch)
    catch_discover_tests(nodesynth_tests)

    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/tests" PREFIX "tests" FILES ${NODESYNTH_TEST_SOURCES})
endif()
