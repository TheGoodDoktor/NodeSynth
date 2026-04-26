#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "dsp/Node.h"

namespace NodeSynth
{
	// Polyphonic voice allocator. Consumes NoteOn / NoteOff events (fed via the
	// audio command queue from UI-thread sources, or via direct in-block calls
	// from FMidiInput) and tracks per-voice state.
	//
	// In Phase 3E-3 the standard output buffers carry voice 0's state only —
	// behaving like a mono node so the allocator is testable and can be patched
	// into the existing graph compiler. Phase 3E-4 extends FAudioGraph::Compile
	// to clone per-voice nodes and route voice-i's clone to voice-i's buffers.
	//
	// Voice stealing is a stub for now (3E-3): allocate the first voice with
	// gate=false. Real same-note retrigger and oldest-released-first stealing
	// land in 3E-5.
	class FVoiceAllocator : public TNodeBase<0, 4>
	{
	public:
		static constexpr size_t MaxVoices = 8;

		enum EParam : uint32_t
		{
			Param_NumVoices,
			Param_Glide,    // future portamento — accepted but unused for now
			Param_COUNT,
		};

		enum EOutput : uint32_t
		{
			Output_Gate,
			Output_Frequency,
			Output_Velocity,
			Output_Note,
		};

		struct FVoice
		{
			uint8_t Note = 0;
			float Velocity = 0.0f;
			bool bGate = false;
			uint64_t AgeSamples = 0;
			uint64_t ReleaseStartedAtSample = 0;
		};

		const char* GetTypeName() const override { return "VoiceAllocator"; }

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
				{ "Note",      EPortType::Control },
			};
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "NumVoices", 0.0f, 3.0f, 3.0f, false, EParamKind::Choice,
					{ "1", "2", "4", "8" },
					"How many simultaneous voices the allocator can play. Changing this triggers a graph recompile." },
				{ "Glide",     0.0f, 2000.0f, 0.0f, true, EParamKind::Float, {},
					"Portamento glide time in milliseconds. Reserved — no effect yet, lands in Phase 4." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_NumVoices: return static_cast<float>(NumVoicesChoice.load(std::memory_order_relaxed));
				case Param_Glide:     return GlideMs.load(std::memory_order_relaxed);
				default:              return 0.0f;
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_NumVoices:
				{
					int32_t V = static_cast<int32_t>(Value);
					if (V < 0) { V = 0; }
					if (V > 3) { V = 3; }
					NumVoicesChoice.store(static_cast<uint8_t>(V), std::memory_order_relaxed);
					break;
				}
				case Param_Glide:
				{
					float V = Value;
					if (V < 0.0f) { V = 0.0f; }
					GlideMs.store(V, std::memory_order_relaxed);
					break;
				}
				default: break;
			}
		}

		// FVoiceAllocator is itself a *source* of polyphony, not a per-voice node
		// that gets cloned. Clone() returns nullptr so the per-voice flag is
		// rejected if the user tries to set it on the allocator itself.
		std::shared_ptr<INode> Clone() const override { return nullptr; }

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
			SampleCounter = 0;
			for (size_t I = 0; I < MaxVoices; ++I)
			{
				Voices[I] = FVoice{};
			}
			for (size_t V = 0; V < MaxVoices; ++V)
			{
				for (uint32_t P = 0; P < NumPorts; ++P)
				{
					for (uint32_t I = 0; I < BlockSize; ++I)
					{
						VoiceBuffers[V][P][I] = 0.0f;
					}
				}
			}
		}

		// Per-voice output buffer accessor used by FGraphModel::Compile to wire
		// voice-i's slice into voice-i's downstream clone. Standard
		// GetOutputBuffer(port) returns voice 0's buffer (back-compat with mono
		// patches that wire the allocator straight into mono nodes).
		float* GetVoiceOutputBuffer(uint32_t PortIndex, size_t VoiceIndex)
		{
			if (PortIndex >= NumPorts || VoiceIndex >= MaxVoices)
			{
				return nullptr;
			}
			return VoiceBuffers[VoiceIndex][PortIndex];
		}

		float* GetOutputBuffer(uint32_t Index) override
		{
			return (Index < NumPorts) ? VoiceBuffers[0][Index] : nullptr;
		}

		// Audio-thread API — called from FAudioGraph::DrainCommands (UI events)
		// or directly by FMidiInput's Process when it drains real MIDI events.
		void HandleNoteOn(uint8_t Note, float Velocity)
		{
			const size_t Active = ActiveVoiceCount();

			// Stub allocation policy: same-note retrigger first, then first
			// voice with gate=false. Full stealing policy lands in 3E-5.
			for (size_t I = 0; I < Active; ++I)
			{
				if (Voices[I].bGate && Voices[I].Note == Note)
				{
					Voices[I].Velocity = Velocity;
					Voices[I].AgeSamples = 0;
					return;
				}
			}
			for (size_t I = 0; I < Active; ++I)
			{
				if (!Voices[I].bGate)
				{
					Voices[I].Note = Note;
					Voices[I].Velocity = Velocity;
					Voices[I].bGate = true;
					Voices[I].AgeSamples = 0;
					return;
				}
			}
			// All voices held — Phase 3E-5 will pick the oldest to steal.
			// For now drop the note silently.
		}

		void HandleNoteOff(uint8_t Note)
		{
			const size_t Active = ActiveVoiceCount();
			for (size_t I = 0; I < Active; ++I)
			{
				if (Voices[I].bGate && Voices[I].Note == Note)
				{
					Voices[I].bGate = false;
					Voices[I].ReleaseStartedAtSample = SampleCounter;
					return;
				}
			}
		}

		void Process(const FProcessContext& Ctx) override
		{
			// Populate every voice's buffer. Per-voice clones routed by Compile
			// pull from voice-i's slice via GetVoiceOutputBuffer; mono patches
			// just see voice 0 via the standard GetOutputBuffer override.
			for (size_t V = 0; V < MaxVoices; ++V)
			{
				const FVoice& Voice = Voices[V];
				const float GateValue = Voice.bGate ? 1.0f : 0.0f;
				const float FreqValue = NoteToFrequency(Voice.Note);
				const float VelValue = Voice.Velocity;
				const float NoteValue = static_cast<float>(Voice.Note);

				float* Gate = VoiceBuffers[V][Output_Gate];
				float* Freq = VoiceBuffers[V][Output_Frequency];
				float* Vel = VoiceBuffers[V][Output_Velocity];
				float* NoteOut = VoiceBuffers[V][Output_Note];
				for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
				{
					Gate[I] = GateValue;
					Freq[I] = FreqValue;
					Vel[I] = VelValue;
					NoteOut[I] = NoteValue;
				}
			}

			// Age voices for stealing-priority bookkeeping (used in 3E-5).
			for (size_t I = 0; I < MaxVoices; ++I)
			{
				Voices[I].AgeSamples += Ctx.BlockSize;
			}
			SampleCounter += Ctx.BlockSize;
		}

		// -- Test / partition-step accessors --------------------------------------
		size_t GetMaxVoices() const { return MaxVoices; }
		size_t ActiveVoiceCount() const
		{
			switch (NumVoicesChoice.load(std::memory_order_relaxed))
			{
				case 0: return 1;
				case 1: return 2;
				case 2: return 4;
				default: return 8;
			}
		}
		const FVoice& GetVoice(size_t Index) const { return Voices[Index]; }

	private:
		static float NoteToFrequency(uint8_t Note)
		{
			return 440.0f * std::pow(2.0f, (static_cast<float>(Note) - 69.0f) / 12.0f);
		}

		std::atomic<uint8_t> NumVoicesChoice{ 3 }; // index into {1,2,4,8} — default 8
		std::atomic<float>   GlideMs{ 0.0f };

		FVoice Voices[MaxVoices]{};

		// Per-voice output storage. Layout: VoiceBuffers[voice][port][sample].
		// MaxVoices × 4 ports × 64 samples × 4 bytes = 8 KiB per allocator.
		static constexpr uint32_t NumPorts = 4;
		alignas(16) float VoiceBuffers[MaxVoices][NumPorts][BlockSize] = {};

		double SampleRate = 48000.0;
		uint64_t SampleCounter = 0;
	};
}
