#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "dsp/Node.h"

namespace NodeSynth
{
	// 16-step sequencer. External clock advances steps; reset jumps back to
	// step 0. Per-step pitch (semitones from RootNote), velocity, gate length
	// (fraction of step duration), and enable. The clock period is measured
	// dynamically — each clock edge records the elapsed sample count, which
	// the gate-length window uses to size its high portion.
	class FSequencer : public TNodeBase<2, 3>
	{
	public:
		static constexpr size_t MaxSteps = 16;

		enum EInput : uint32_t
		{
			Input_Clock,
			Input_Reset,
		};

		enum EOutput : uint32_t
		{
			Output_Gate,
			Output_Frequency,
			Output_Velocity,
		};

		// Param layout: 2 globals, then 4 sub-fields × MaxSteps.
		enum EParam : uint32_t
		{
			Param_NumSteps,
			Param_RootNote,
			Param_StepPitchBase,                              // [..+MaxSteps)
			Param_StepVelocityBase = Param_StepPitchBase + MaxSteps,
			Param_StepGateLengthBase = Param_StepVelocityBase + MaxSteps,
			Param_StepEnabledBase = Param_StepGateLengthBase + MaxSteps,
			Param_COUNT = Param_StepEnabledBase + MaxSteps,
		};

		FSequencer()
		{
			// Atomics default-initialise to 0; bring the per-step defaults
			// in line with what GetParamInfos() advertises.
			for (size_t I = 0; I < MaxSteps; ++I)
			{
				StepVelocity[I].store(0.8f, std::memory_order_relaxed);
				StepGateLength[I].store(0.5f, std::memory_order_relaxed);
				StepEnabled[I].store(true, std::memory_order_relaxed);
			}
		}

		const char* GetTypeName() const override { return "Sequencer"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return
			{
				{ "Clock", EPortType::Control,
					"Rising-edge advances to the next step." },
				{ "Reset", EPortType::Control,
					"Rising-edge jumps back to step 0." },
			};
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return
			{
				{ "Gate",      EPortType::Control,
					"1.0 during the gate-length window of each enabled step; 0.0 otherwise." },
				{ "Frequency", EPortType::Control,
					"Pitch of the active step in Hz (RootNote + StepPitch semitones)." },
				{ "Velocity",  EPortType::Control,
					"Velocity of the active step (0..1)." },
			};
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			std::vector<FParamInfo> Infos;
			Infos.reserve(Param_COUNT);

			Infos.push_back({ "NumSteps", 1.0f, 16.0f, 16.0f, false, EParamKind::Float, {},
				"How many steps the sequencer cycles through before looping. Steps beyond this index are inactive.",
				false });
			Infos.push_back({ "RootNote", 0.0f, 127.0f, 60.0f, false, EParamKind::Float, {},
				"MIDI note number that StepPitch=0 maps to. Default 60 = middle C (C4).",
				false });

			for (size_t I = 0; I < MaxSteps; ++I)
			{
				Infos.push_back({ "StepPitch" + std::to_string(I),
					-24.0f, 24.0f, 0.0f, false, EParamKind::Float, {},
					"Semitone offset for this step.", true });
			}
			for (size_t I = 0; I < MaxSteps; ++I)
			{
				Infos.push_back({ "StepVelocity" + std::to_string(I),
					0.0f, 1.0f, 0.8f, false, EParamKind::Float, {},
					"Velocity for this step.", true });
			}
			for (size_t I = 0; I < MaxSteps; ++I)
			{
				Infos.push_back({ "StepGateLength" + std::to_string(I),
					0.0f, 1.0f, 0.5f, false, EParamKind::Float, {},
					"Gate-high fraction of the step duration (0..1).", true });
			}
			for (size_t I = 0; I < MaxSteps; ++I)
			{
				Infos.push_back({ "StepEnabled" + std::to_string(I),
					0.0f, 1.0f, 1.0f, false, EParamKind::Bool, {},
					"When unchecked, the step's gate stays low and is effectively skipped audibly.", true });
			}
			return Infos;
		}

		float GetParamValue(uint32_t Index) const override
		{
			if (Index == Param_NumSteps) { return static_cast<float>(NumSteps.load(std::memory_order_relaxed)); }
			if (Index == Param_RootNote) { return RootNote.load(std::memory_order_relaxed); }
			if (Index >= Param_StepPitchBase && Index < Param_StepPitchBase + MaxSteps)
			{
				return StepPitch[Index - Param_StepPitchBase].load(std::memory_order_relaxed);
			}
			if (Index >= Param_StepVelocityBase && Index < Param_StepVelocityBase + MaxSteps)
			{
				return StepVelocity[Index - Param_StepVelocityBase].load(std::memory_order_relaxed);
			}
			if (Index >= Param_StepGateLengthBase && Index < Param_StepGateLengthBase + MaxSteps)
			{
				return StepGateLength[Index - Param_StepGateLengthBase].load(std::memory_order_relaxed);
			}
			if (Index >= Param_StepEnabledBase && Index < Param_StepEnabledBase + MaxSteps)
			{
				return StepEnabled[Index - Param_StepEnabledBase].load(std::memory_order_relaxed) ? 1.0f : 0.0f;
			}
			return 0.0f;
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			if (Index == Param_NumSteps)
			{
				int32_t V = static_cast<int32_t>(std::lround(Value));
				if (V < 1) { V = 1; }
				if (V > static_cast<int32_t>(MaxSteps)) { V = static_cast<int32_t>(MaxSteps); }
				NumSteps.store(static_cast<uint32_t>(V), std::memory_order_relaxed);
				return;
			}
			if (Index == Param_RootNote)
			{
				float V = Value;
				if (V < 0.0f) { V = 0.0f; }
				if (V > 127.0f) { V = 127.0f; }
				RootNote.store(V, std::memory_order_relaxed);
				return;
			}
			if (Index >= Param_StepPitchBase && Index < Param_StepPitchBase + MaxSteps)
			{
				float V = Value;
				if (V < -24.0f) { V = -24.0f; }
				if (V > 24.0f) { V = 24.0f; }
				StepPitch[Index - Param_StepPitchBase].store(V, std::memory_order_relaxed);
				return;
			}
			if (Index >= Param_StepVelocityBase && Index < Param_StepVelocityBase + MaxSteps)
			{
				float V = Value;
				if (V < 0.0f) { V = 0.0f; }
				if (V > 1.0f) { V = 1.0f; }
				StepVelocity[Index - Param_StepVelocityBase].store(V, std::memory_order_relaxed);
				return;
			}
			if (Index >= Param_StepGateLengthBase && Index < Param_StepGateLengthBase + MaxSteps)
			{
				float V = Value;
				if (V < 0.0f) { V = 0.0f; }
				if (V > 1.0f) { V = 1.0f; }
				StepGateLength[Index - Param_StepGateLengthBase].store(V, std::memory_order_relaxed);
				return;
			}
			if (Index >= Param_StepEnabledBase && Index < Param_StepEnabledBase + MaxSteps)
			{
				StepEnabled[Index - Param_StepEnabledBase].store(Value > 0.5f, std::memory_order_relaxed);
				return;
			}
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
			CurrentStep = 0;
			SamplesIntoStep = 0;
			SamplesPerStep = static_cast<uint64_t>(InSampleRate * 0.5);  // 120 BPM default
			bPrevClockHigh = false;
			bPrevResetHigh = false;
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Gate = GetOutputBuffer(Output_Gate);
			float* Freq = GetOutputBuffer(Output_Frequency);
			float* Vel = GetOutputBuffer(Output_Velocity);
			const float* Clock = GetInputBuffer(Input_Clock);
			const float* Reset = GetInputBuffer(Input_Reset);

			const uint32_t NumStepsNow = NumSteps.load(std::memory_order_relaxed);
			const float RootNoteNow = RootNote.load(std::memory_order_relaxed);

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				// Reset edge.
				const float ResetV = (Reset != nullptr) ? Reset[I] : 0.0f;
				const bool bResetHigh = ResetV > 0.5f;
				if (bResetHigh && !bPrevResetHigh)
				{
					CurrentStep = 0;
					SamplesIntoStep = 0;
				}
				bPrevResetHigh = bResetHigh;

				// Clock edge — advance step. Record period for gate sizing.
				const float ClockV = (Clock != nullptr) ? Clock[I] : 0.0f;
				const bool bClockHigh = ClockV > 0.5f;
				if (bClockHigh && !bPrevClockHigh)
				{
					if (SamplesIntoStep > 0)
					{
						SamplesPerStep = SamplesIntoStep;
					}
					CurrentStep = (CurrentStep + 1) % NumStepsNow;
					SamplesIntoStep = 0;
				}
				bPrevClockHigh = bClockHigh;

				// Output the active step's signals.
				const size_t Step = CurrentStep;
				const float StepPitchNow = StepPitch[Step].load(std::memory_order_relaxed);
				const float StepVelNow = StepVelocity[Step].load(std::memory_order_relaxed);
				const float StepGateLenNow = StepGateLength[Step].load(std::memory_order_relaxed);
				const bool bStepEnabledNow = StepEnabled[Step].load(std::memory_order_relaxed);

				const float MidiNote = RootNoteNow + StepPitchNow;
				Freq[I] = NoteToFrequency(MidiNote);
				Vel[I] = StepVelNow;

				const uint64_t GateSamples = static_cast<uint64_t>(
					static_cast<double>(SamplesPerStep) * StepGateLenNow);
				const bool bGateHigh = bStepEnabledNow && (SamplesIntoStep < GateSamples);
				Gate[I] = bGateHigh ? 1.0f : 0.0f;

				++SamplesIntoStep;
			}
		}

		// Test accessors.
		size_t GetCurrentStep() const { return CurrentStep; }
		uint64_t GetSamplesPerStep() const { return SamplesPerStep; }

	private:
		static float NoteToFrequency(float Note)
		{
			return 440.0f * std::pow(2.0f, (Note - 69.0f) / 12.0f);
		}

		std::atomic<uint32_t> NumSteps{ 16 };
		std::atomic<float>    RootNote{ 60.0f };

		std::array<std::atomic<float>, MaxSteps> StepPitch{};
		std::array<std::atomic<float>, MaxSteps> StepVelocity{};
		std::array<std::atomic<float>, MaxSteps> StepGateLength{};
		std::array<std::atomic<bool>,  MaxSteps> StepEnabled{};

		double SampleRate = 48000.0;
		size_t CurrentStep = 0;
		uint64_t SamplesIntoStep = 0;
		uint64_t SamplesPerStep = 24000;
		bool bPrevClockHigh = false;
		bool bPrevResetHigh = false;
	};
}
