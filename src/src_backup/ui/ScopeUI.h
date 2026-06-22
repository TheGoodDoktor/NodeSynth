#pragma once

namespace NodeSynth
{
	class FScope;

	// Draws the scope's polyline in the property panel using the most recent
	// WindowSize samples from the audio-thread ring.
	void DrawScopeUI(FScope& Scope);
}
