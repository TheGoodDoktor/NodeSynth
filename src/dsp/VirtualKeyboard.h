#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	// On-screen keyboard for testing the synth without a physical MIDI controller.
	// One displayed octave (13 keys, C..C) plus an octave-shift param and a
	// modulation slider. Drop-in replacement for FMidiInput's first three outputs;
	// adds a fourth (ModWheel) that FMidiInput will grow alongside CC handling later.
	//
	// Threading: UI thread mutates the held-note stack via PressNote/ReleaseNote.
	// Audio thread only reads the cross-thread atomics during Process(). No SPSC
	// ring is needed — input is produced at human/UI-frame rates and missing
	// intermediate states is harmless. Drawing lives in src/ui/VirtualKeyboardUI.cpp;
	// this header stays imgui-free so it compiles cleanly into the test binary.
	class FVirtualKeyboard : public TNodeBase<0, 4>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Octave,
			Param_Velocity,
			Param_ModWheel,
			Param_COUNT,
		};

		enum EOutput : uint32_t
		{
			Output_Gate,
			Output_Frequency,
			Output_Velocity,
			Output_ModWheel,
		};

		const char* GetTypeName() const override { return "VirtualKbd"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return {};
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return
			{
				{ "Gate",      EPortType::Control },
				{ "Frequency", EPortType::Control },
				{ "Velocity",  EPortType::Control },
				{ "ModWheel",  EPortType::Control },
			};
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Octave",   0.0f, 8.0f, 4.0f, false, EParamKind::Float, {} },
				{ "Velocity", 0.0f, 1.0f, 0.8f, false, EParamKind::Float, {} },
				{ "Mod",      0.0f, 1.0f, 0.0f, false, EParamKind::Float, {} },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_Octave:   return static_cast<float>(Octave.load(std::memory_order_relaxed));
				case Param_Velocity: return Velocity.load(std::memory_order_relaxed);
				case Param_ModWheel: return ModWheel.load(std::memory_order_relaxed);
				default:             return 0.0f;
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_Octave:
				{
					int32_t V = static_cast<int32_t>(std::lround(Value));
					if (V < 0) { V = 0; }
					if (V > 8) { V = 8; }
					Octave.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_Velocity:
				{
					float V = Value;
					if (V < 0.0f) { V = 0.0f; }
					if (V > 1.0f) { V = 1.0f; }
					Velocity.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_ModWheel:
				{
					float V = Value;
					if (V < 0.0f) { V = 0.0f; }
					if (V > 1.0f) { V = 1.0f; }
					ModWheel.store(V, std::memory_order_relaxed);
					break;
				}
				default: break;
			}
		}

		void Prepare(double InSampleRate) override
		{
			ModSmoother.Prepare(InSampleRate);
			ModSmoother.Reset(ModWheel.load(std::memory_order_relaxed));
		}

		void Process(const FProcessContext& Ctx) override
		{
			const float Gate = bGate.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
			const float Freq = CurrentFreq.load(std::memory_order_relaxed);
			const float Vel = Velocity.load(std::memory_order_relaxed);
			ModSmoother.SetTarget(ModWheel.load(std::memory_order_relaxed));

			float* GateOut = GetOutputBuffer(Output_Gate);
			float* FreqOut = GetOutputBuffer(Output_Frequency);
			float* VelOut = GetOutputBuffer(Output_Velocity);
			float* ModOut = GetOutputBuffer(Output_ModWheel);

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				GateOut[I] = Gate;
				FreqOut[I] = Freq;
				VelOut[I] = Vel * Gate;
				ModOut[I] = ModSmoother.Tick();
			}
		}

		// -- UI-thread interface -------------------------------------------------
		// SemitoneFromBottomC: 0..12 (low C through high C of the displayed octave).
		// Mapped to MIDI via the current Octave param: MIDI = 12 * (Octave + 1) + Semi.
		void PressNote(int32_t SemitoneFromBottomC)
		{
			const int32_t MidiNote = SemitoneToMidi(SemitoneFromBottomC);
			if (MidiNote < 0 || MidiNote > 127)
			{
				return;
			}
			const uint8_t Note = static_cast<uint8_t>(MidiNote);

			for (size_t I = 0; I < NumHeldNotes; ++I)
			{
				if (HeldNotes[I] == Note)
				{
					return;
				}
			}
			if (NumHeldNotes >= MaxHeldNotes)
			{
				return;
			}

			HeldNotes[NumHeldNotes] = Note;
			HeldSemitones[NumHeldNotes] = static_cast<int8_t>(SemitoneFromBottomC);
			++NumHeldNotes;

			CurrentFreq.store(NoteToFrequency(Note), std::memory_order_relaxed);
			bGate.store(true, std::memory_order_relaxed);
		}

		void ReleaseNote(int32_t SemitoneFromBottomC)
		{
			for (size_t I = 0; I < NumHeldNotes; ++I)
			{
				if (HeldSemitones[I] == static_cast<int8_t>(SemitoneFromBottomC))
				{
					for (size_t J = I + 1; J < NumHeldNotes; ++J)
					{
						HeldNotes[J - 1] = HeldNotes[J];
						HeldSemitones[J - 1] = HeldSemitones[J];
					}
					--NumHeldNotes;
					break;
				}
			}
			if (NumHeldNotes == 0)
			{
				bGate.store(false, std::memory_order_relaxed);
			}
			else
			{
				const uint8_t Top = HeldNotes[NumHeldNotes - 1];
				CurrentFreq.store(NoteToFrequency(Top), std::memory_order_relaxed);
			}
		}

		void ReleaseAll()
		{
			NumHeldNotes = 0;
			bGate.store(false, std::memory_order_relaxed);
		}

		// UI-thread query — used by the property-panel drawing for visual feedback.
		bool IsKeyHeld(int32_t SemitoneFromBottomC) const
		{
			for (size_t I = 0; I < NumHeldNotes; ++I)
			{
				if (HeldSemitones[I] == static_cast<int8_t>(SemitoneFromBottomC))
				{
					return true;
				}
			}
			return false;
		}

		size_t GetNumHeldNotes() const { return NumHeldNotes; }

	private:
		int32_t SemitoneToMidi(int32_t Semi) const
		{
			const int32_t Octv = Octave.load(std::memory_order_relaxed);
			return 12 * (Octv + 1) + Semi;
		}

		static float NoteToFrequency(uint8_t Note)
		{
			return 440.0f * std::pow(2.0f, (static_cast<float>(Note) - 69.0f) / 12.0f);
		}

		static constexpr size_t MaxHeldNotes = 16;

		// UI-thread-only state. Held semitones are tracked in parallel so that
		// ReleaseNote works correctly across an Octave change made while keys are held:
		// the user releases the same UI key they pressed, even if the underlying MIDI
		// note has shifted.
		uint8_t HeldNotes[MaxHeldNotes] = {};
		int8_t HeldSemitones[MaxHeldNotes] = {};
		size_t NumHeldNotes = 0;

		// Cross-thread state.
		std::atomic<bool>    bGate{ false };
		std::atomic<float>   CurrentFreq{ 0.0f };
		std::atomic<int32_t> Octave{ 4 };
		std::atomic<float>   Velocity{ 0.8f };
		std::atomic<float>   ModWheel{ 0.0f };

		// Audio-thread-only.
		FOnePoleSmoother ModSmoother;
	};
}
