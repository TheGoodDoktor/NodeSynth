#include "ui/SequencerUI.h"

#include <algorithm>
#include <cstdio>

#include <imgui.h>

#include "dsp/Sequencer.h"

namespace NodeSynth
{
	void DrawSequencerUI(FSequencer& Seq, const FCommandSink& Sink)
	{
		auto WriteParam = [&](uint32_t Index, float Value)
		{
			Seq.SetParamValue(Index, Value);
			Sink.SetParam(Index, Value);
		};

		ImGui::Separator();
		ImGui::TextUnformatted("Steps");

		const float Scale = ImGui::GetFontSize() / 13.0f;
		const float CellWidth = 28.0f * Scale;
		const float PitchHeight = 80.0f * Scale;
		const float VelocityHeight = 12.0f * Scale;
		const size_t NumStepsActive = static_cast<size_t>(
			std::lround(Seq.GetParamValue(FSequencer::Param_NumSteps)));
		const size_t Current = Seq.GetCurrentStep();

		ImDrawList* Draw = ImGui::GetWindowDrawList();

		// Lay out 16 columns. Each column has: enable checkbox, pitch
		// vertical slider, velocity horizontal bar, gate-length bar.
		ImGui::BeginGroup();
		for (size_t I = 0; I < FSequencer::MaxSteps; ++I)
		{
			ImGui::PushID(static_cast<int>(I));

			const ImVec2 ColTop = ImGui::GetCursorScreenPos();
			const bool bIsActiveStep = (I < NumStepsActive);
			const bool bIsCurrent = (I == Current) && bIsActiveStep;

			// Highlight the current step with a subtle column background.
			if (bIsCurrent)
			{
				const float ColH = (PitchHeight + VelocityHeight + VelocityHeight + 50.0f * Scale);
				Draw->AddRectFilled(
					ImVec2(ColTop.x - 2.0f, ColTop.y - 2.0f),
					ImVec2(ColTop.x + CellWidth + 2.0f, ColTop.y + ColH),
					IM_COL32(120, 180, 255, 60), 3.0f);
			}

			// Enable checkbox.
			ImGui::BeginDisabled(!bIsActiveStep);
			bool bEnabled = Seq.GetParamValue(FSequencer::Param_StepEnabledBase + I) > 0.5f;
			if (ImGui::Checkbox("##en", &bEnabled))
			{
				WriteParam(FSequencer::Param_StepEnabledBase + I, bEnabled ? 1.0f : 0.0f);
			}

			// Vertical pitch slider (-24..+24 semitones from RootNote).
			float Pitch = Seq.GetParamValue(FSequencer::Param_StepPitchBase + I);
			if (ImGui::VSliderFloat("##pitch", ImVec2(CellWidth - 4.0f, PitchHeight),
				&Pitch, -24.0f, 24.0f, "%+.0f"))
			{
				WriteParam(FSequencer::Param_StepPitchBase + I, Pitch);
			}

			// Velocity horizontal bar (drag to set 0..1).
			float Vel = Seq.GetParamValue(FSequencer::Param_StepVelocityBase + I);
			if (ImGui::SliderFloat("##vel", &Vel, 0.0f, 1.0f, ""))
			{
				WriteParam(FSequencer::Param_StepVelocityBase + I, Vel);
			}

			// Gate-length horizontal bar (drag to set 0..1).
			float GateLen = Seq.GetParamValue(FSequencer::Param_StepGateLengthBase + I);
			if (ImGui::SliderFloat("##gate", &GateLen, 0.0f, 1.0f, ""))
			{
				WriteParam(FSequencer::Param_StepGateLengthBase + I, GateLen);
			}

			ImGui::EndDisabled();
			ImGui::PopID();
			if (I < FSequencer::MaxSteps - 1)
			{
				ImGui::SameLine();
			}
		}
		ImGui::EndGroup();

		// Legend.
		ImGui::TextDisabled("Top to bottom: enable, pitch (semitones), velocity, gate length.");
	}
}
