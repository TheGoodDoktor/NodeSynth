#pragma once

struct ImDrawList;
struct ImVec2;

namespace NodeSynth
{
	// Draws a category-coded icon for the given node type into the given screen-space
	// rectangle. Used in the graph node headers and the create-node menu so node types
	// are recognisable at a glance. TypeName matches INode::GetTypeName().
	void DrawNodeIcon(const char* TypeName, ImDrawList* Draw, const ImVec2& Min, const ImVec2& Max);

	// Equivalent to placing a DrawNodeIcon at the current cursor, advancing the cursor
	// past it, then SameLine() so subsequent text/widgets sit next to the icon.
	// Size matches ImGui::GetFrameHeight() for menu rows or GetTextLineHeight() for compact use.
	void IconBeforeText(const char* TypeName, float Size);
}
