#pragma once

#include "dsp/Node.h"

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

	// Draws a port icon (circle for Audio, diamond for Control) at the current
	// cursor with the given size. Filled when the pin is connected, outlined
	// otherwise. Colour scheme: Audio = warm yellow-orange, Control = teal.
	// Advances the cursor past the icon and calls SameLine() so subsequent
	// text sits to the right.
	void DrawPinIcon(EPortType Type, bool bConnected, float Size);

	// Returns a packed ImU32 colour matching the category accent the node
	// editor's title-bar tint uses. Same colour as the icon dispatch in
	// DrawNodeIcon, so glyph and header agree visually. Default-category
	// fallback for unknown types.
	unsigned int GetCategoryColor(const char* TypeName);
}
