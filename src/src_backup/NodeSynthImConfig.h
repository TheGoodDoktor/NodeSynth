#pragma once

// Project-wide override of Dear ImGui's imconfig.h (selected via IMGUI_USER_CONFIG).
// We enable IMGUI_DEFINE_MATH_OPERATORS here so every TU sees the same ImVec2
// operator surface — imgui-node-editor's internal headers assume the operators
// are available by the time <imgui.h> is first included.

#define IMGUI_DEFINE_MATH_OPERATORS
