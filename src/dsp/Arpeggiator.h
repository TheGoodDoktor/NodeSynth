#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "dsp/Node.h"
#include "dsp/NoteSink.h"

namespace NodeSynth
{
	// Monophonic arpeggiator. Captures the set of currently-held notes (via the
	// INoteSink note-event path, exactly like FVoiceAllocator) and plays them
	// back one at a time in a chosen order at a clocked rate, emitting the same
	// Gate / Frequency / Velocity / Note Control outputs as the voice allocator
	// and sequencer — so it drops straight into the note → synth chain in place
	// of the allocator.
	//
	// Clock source: when the Clock input is connected a rising edge advances one
	// step (overriding the internal clock); otherwise the internal BPM × Rate
	// clock runs free. Reset rising edge jumps back before step 0.
	//
	// Note events arrive on the audio thread only (FMidiDeviceManager::Process
	// and FAudioGraph::DrainCommands, both before FAudioGraph::Process), so the
	// held-note set needs no locking. Params are atomics because slider drags
	// push SetParam through the command ring.
	class FArpeggiator : public TNodeBase<2, 4>, public INoteSink
	{
	public:
		static constexpr size_t MaxHeld = 16;
		static constexpr size_t MaxOctaves = 4;
		static constexpr size_t MaxExpanded = MaxHeld * MaxOctaves;

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
			Output_Note,
		};

		enum EParam : uint32_t
		{
			Param_Pattern,
			Param_Bpm,
			Param_Rate,
			Param_Octaves,
			Param_Gate,
			Param_Swing,
			Param_Latch,
			Param_COUNT,
		};

		enum EPattern : uint8_t
		{
			Pattern_Up,
			Pattern_Down,
			Pattern_UpDown,
			Pattern_DownUp,
			Pattern_AsPlayed,
			Pattern_Random,
		};

		const char* GetTypeName() const override { return "Arpeggiator"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return
			{
				{ "Clock", EPortType::Control,
					"When connected, a rising edge advances one step (overrides the\n"
					"internal BPM/Rate clock). Wire a Clock node here to sync." },
				{ "Reset", EPortType::Control,
					"Rising edge restarts the pattern from step 0." },
			};
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return
			{
				{ "Gate",      EPortType::Control,
					"1.0 during the gate-length window of the active step; drive an ADSR with this." },
				{ "Frequency", EPortType::Control,
					"Frequency of the active step's note in Hz." },
				{ "Velocity",  EPortType::Control,
					"Captured velocity of the active step's note (0..1)." },
				{ "Note",      EPortType::Control,
					"MIDI note number of the active step (0..127)." },
			};
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Pattern", 0.0f, 5.0f, 0.0f, false, EParamKind::Choice,
					{ "Up", "Down", "Up/Down", "Down/Up", "As Played", "Random" },
					"Order in which held notes are played." },
				{ "BPM", 20.0f, 300.0f, 120.0f, false, EParamKind::Float, {},
					"Internal-clock tempo. Ignored when the Clock input is connected." },
				{ "Rate", 0.0f, 5.0f, 3.0f, false, EParamKind::Choice,
					{ "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" },
					"Steps per beat for the internal clock. Ignored when the Clock input is connected." },
				{ "Octaves", 0.0f, 3.0f, 0.0f, false, EParamKind::Choice,
					{ "1", "2", "3", "4" },
					"How many octaves the pattern spans. The held set is repeated, transposed up 12 semitones per extra octave." },
				{ "Gate", 0.0f, 1.0f, 0.5f, false, EParamKind::Float, {},
					"Gate-high fraction of each step (0..1). Lower values give a more staccato, plucky feel." },
				{ "Swing", 0.0f, 0.75f, 0.0f, false, EParamKind::Float, {},
					"Shuffle feel: lengthens even steps and shortens odd ones by this fraction." },
				{ "Latch", 0.0f, 1.0f, 0.0f, false, EParamKind::Bool, {},
					"When on, the pattern keeps running after all keys are released; the next\n"
					"note pressed (after release) starts a fresh chord." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_Pattern: return static_cast<float>(Pattern.load(std::memory_order_relaxed));
				case Param_Bpm:     return Bpm.load(std::memory_order_relaxed);
				case Param_Rate:    return static_cast<float>(RateChoice.load(std::memory_order_relaxed));
				case Param_Octaves: return static_cast<float>(OctavesChoice.load(std::memory_order_relaxed));
				case Param_Gate:    return GateFrac.load(std::memory_order_relaxed);
				case Param_Swing:   return Swing.load(std::memory_order_relaxed);
				case Param_Latch:   return Latch.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
				default:            return 0.0f;
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_Pattern:
				{
					int32_t V = static_cast<int32_t>(std::lround(Value));
					if (V < 0) { V = 0; }
					if (V > 5) { V = 5; }
					Pattern.store(static_cast<uint8_t>(V), std::memory_order_relaxed);
					break;
				}
				case Param_Bpm:
				{
					float V = Value;
					if (V < 20.0f) { V = 20.0f; }
					if (V > 300.0f) { V = 300.0f; }
					Bpm.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_Rate:
				{
					int32_t V = static_cast<int32_t>(std::lround(Value));
					if (V < 0) { V = 0; }
					if (V > 5) { V = 5; }
					RateChoice.store(static_cast<uint8_t>(V), std::memory_order_relaxed);
					break;
				}
				case Param_Octaves:
				{
					int32_t V = static_cast<int32_t>(std::lround(Value));
					if (V < 0) { V = 0; }
					if (V > 3) { V = 3; }
					OctavesChoice.store(static_cast<uint8_t>(V), std::memory_order_relaxed);
					break;
				}
				case Param_Gate:
				{
					float V = Value;
					if (V < 0.0f) { V = 0.0f; }
					if (V > 1.0f) { V = 1.0f; }
					GateFrac.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_Swing:
				{
					float V = Value;
					if (V < 0.0f) { V = 0.0f; }
					if (V > 0.75f) { V = 0.75f; }
					Swing.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_Latch:
				{
					Latch.store(Value > 0.5f, std::memory_order_relaxed);
					break;
				}
				default: break;
			}
		}

		// The arpeggiator is a polyphony source (like FVoiceAllocator), never a
		// per-voice clone — reject the per-voice flag by returning nullptr.
		std::shared_ptr<INode> Clone() const override { return nullptr; }

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
			HeldCount = 0;
			PhysicalKeysDown = 0;
			for (size_t I = 0; I < 128; ++I) { bPhysDown[I] = false; }
			StepIndex = -1;
			SamplesIntoStep = 0;
			BaseSamplesPerStep = ComputeInternalStepSamples();
			SamplesPerStep = static_cast<uint64_t>(BaseSamplesPerStep > 1.0 ? BaseSamplesPerStep : 1.0);
			bPrevClockHigh = false;
			bPrevResetHigh = false;
			bPendingRetrigger = false;
			CurNoteValid = false;
			CurNote = 69;
			CurVel = 0.0f;
			LastNote = 69;
			LastFreq = NoteToFrequency(69);
			ResolvedLen = 0;
			RandState = 0x9E3779B9u;  // fixed seed — no Math.random in the audio path
		}

		// -- INoteSink (audio thread) ---------------------------------------------
		void HandleNoteOn(uint8_t Note, float Velocity) override
		{
			if (Note > 127) { return; }
			const bool bLatch = Latch.load(std::memory_order_relaxed);

			// Track the physical key.
			if (!bPhysDown[Note])
			{
				bPhysDown[Note] = true;
				++PhysicalKeysDown;
			}

			// Latch: the first key pressed after everything was released starts a
			// fresh phrase, clearing the previously-latched set.
			if (bLatch && (PhysicalKeysDown == 1))
			{
				HeldCount = 0;
			}

			const bool bWasEmpty = (HeldCount == 0);
			AddHeld(Note, Velocity);

			// Fresh-chord retrigger: empty → non-empty restarts the pattern and
			// (internal clock) fires step 0 promptly rather than waiting a step.
			if (bWasEmpty)
			{
				StepIndex = -1;
				bPendingRetrigger = true;
			}
		}

		void HandleNoteOff(uint8_t Note) override
		{
			if (Note > 127) { return; }
			if (bPhysDown[Note])
			{
				bPhysDown[Note] = false;
				if (PhysicalKeysDown > 0) { --PhysicalKeysDown; }
			}
			// Latch sustains released notes in the playing set until a fresh
			// phrase begins; non-latch removes them immediately.
			if (!Latch.load(std::memory_order_relaxed))
			{
				RemoveHeld(Note);
			}
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Gate    = GetOutputBuffer(Output_Gate);
			float* Freq    = GetOutputBuffer(Output_Frequency);
			float* Vel     = GetOutputBuffer(Output_Velocity);
			float* NoteOut = GetOutputBuffer(Output_Note);
			const float* Clock = GetInputBuffer(Input_Clock);
			const float* Reset = GetInputBuffer(Input_Reset);

			// If latch was turned off while a phrase was sustaining with no keys
			// down, drop the latched set so the pattern stops.
			if (!Latch.load(std::memory_order_relaxed) && PhysicalKeysDown == 0 && HeldCount > 0)
			{
				HeldCount = 0;
			}
			// All keys up (non-latch) → silence immediately rather than holding
			// the current note for the rest of its step.
			if (HeldCount == 0)
			{
				CurNoteValid = false;
				StepIndex = -1;
			}

			// Honour a pending fresh-chord retrigger: internal clock fires the
			// first step at once; external clock waits for the next edge.
			if (bPendingRetrigger)
			{
				if (Clock == nullptr)
				{
					SamplesIntoStep = SamplesPerStep;  // force advance on the first sample
				}
				bPendingRetrigger = false;
			}

			const float GateNow = GateFrac.load(std::memory_order_relaxed);

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				// Reset edge → restart pattern.
				const float ResetV = (Reset != nullptr) ? Reset[I] : 0.0f;
				const bool bResetHigh = ResetV > 0.5f;
				if (bResetHigh && !bPrevResetHigh)
				{
					StepIndex = -1;
					SamplesIntoStep = 0;
				}
				bPrevResetHigh = bResetHigh;

				// Decide whether to advance a step this sample.
				bool bAdvance = false;
				if (Clock != nullptr)
				{
					const bool bClockHigh = Clock[I] > 0.5f;
					if (bClockHigh && !bPrevClockHigh) { bAdvance = true; }
					bPrevClockHigh = bClockHigh;
				}
				else
				{
					if (SamplesIntoStep >= SamplesPerStep) { bAdvance = true; }
				}

				if (bAdvance)
				{
					// Establish the base step length: external clock measures the
					// elapsed period between edges; internal clock derives it from
					// BPM × Rate.
					if (Clock != nullptr)
					{
						if (SamplesIntoStep > 0)
						{
							BaseSamplesPerStep = static_cast<double>(SamplesIntoStep);
						}
					}
					else
					{
						BaseSamplesPerStep = ComputeInternalStepSamples();
					}

					BuildResolved();
					const int32_t P = PatternPeriod();
					if (P <= 0)
					{
						StepIndex = -1;
						CurNoteValid = false;
					}
					else
					{
						StepIndex = (StepIndex < 0) ? 0 : ((StepIndex + 1) % P);
						const int32_t E = PositionToExpandedIndex(StepIndex);
						CurNote = ExpNote[E];
						CurVel = ExpVel[E];
						CurNoteValid = true;
						LastNote = CurNote;
						LastFreq = NoteToFrequency(CurNote);
					}

					// Swing: lengthen even steps, shorten odd ones, keeping each
					// pair's total constant.
					const float Sw = Swing.load(std::memory_order_relaxed);
					const float Factor = (StepIndex >= 0 && (StepIndex % 2) == 0)
						? (1.0f + Sw) : (1.0f - Sw);
					double Len = BaseSamplesPerStep * static_cast<double>(Factor);
					if (Len < 1.0) { Len = 1.0; }
					SamplesPerStep = static_cast<uint64_t>(Len);
					SamplesIntoStep = 0;
				}

				if (CurNoteValid)
				{
					Freq[I] = LastFreq;
					Vel[I] = CurVel;
					NoteOut[I] = static_cast<float>(CurNote);
					const uint64_t GateSamples = static_cast<uint64_t>(
						static_cast<double>(SamplesPerStep) * GateNow);
					Gate[I] = (SamplesIntoStep < GateSamples) ? 1.0f : 0.0f;
				}
				else
				{
					Freq[I] = LastFreq;        // hold last pitch to avoid downstream clicks
					Vel[I] = 0.0f;
					NoteOut[I] = static_cast<float>(LastNote);
					Gate[I] = 0.0f;
				}

				++SamplesIntoStep;
			}
		}

		// -- Test accessors -------------------------------------------------------
		size_t GetHeldCount() const { return HeldCount; }
		int32_t GetStepIndex() const { return StepIndex; }
		int32_t GetResolvedLength() const { return ResolvedLen; }
		uint64_t GetSamplesPerStep() const { return SamplesPerStep; }
		bool IsCurrentNoteValid() const { return CurNoteValid; }
		uint8_t GetCurrentNote() const { return CurNote; }

	private:
		static float NoteToFrequency(uint8_t Note)
		{
			return 440.0f * std::pow(2.0f, (static_cast<float>(Note) - 69.0f) / 12.0f);
		}

		double ComputeInternalStepSamples() const
		{
			static constexpr int32_t StepsPerBeat[6] = { 1, 2, 3, 4, 6, 8 };
			const float BpmNow = Bpm.load(std::memory_order_relaxed);
			const int32_t RateIdx = RateChoice.load(std::memory_order_relaxed);
			const int32_t PerBeat = StepsPerBeat[RateIdx];
			const double SamplesPerBeat = SampleRate * 60.0 / static_cast<double>(BpmNow);
			const double Result = SamplesPerBeat / static_cast<double>(PerBeat);
			return (Result > 1.0) ? Result : 1.0;
		}

		int32_t OctaveCount() const
		{
			return static_cast<int32_t>(OctavesChoice.load(std::memory_order_relaxed)) + 1;
		}

		void AddHeld(uint8_t Note, float Velocity)
		{
			// Same-note refresh: update velocity, keep position.
			for (size_t I = 0; I < HeldCount; ++I)
			{
				if (Held[I].Note == Note)
				{
					Held[I].Velocity = Velocity;
					return;
				}
			}
			if (HeldCount >= MaxHeld) { return; }
			Held[HeldCount].Note = Note;
			Held[HeldCount].Velocity = Velocity;
			Held[HeldCount].Order = InsertionCounter++;
			++HeldCount;
		}

		void RemoveHeld(uint8_t Note)
		{
			for (size_t I = 0; I < HeldCount; ++I)
			{
				if (Held[I].Note == Note)
				{
					for (size_t J = I + 1; J < HeldCount; ++J)
					{
						Held[J - 1] = Held[J];
					}
					--HeldCount;
					return;
				}
			}
		}

		// Rebuild the expanded note/velocity arrays from the held set, the
		// pattern's single-octave order, and the octave count. Called on each
		// step advance (HeldCount ≤ 16, so this is cheap).
		void BuildResolved()
		{
			const EPattern Pat = static_cast<EPattern>(Pattern.load(std::memory_order_relaxed));
			const int32_t N = static_cast<int32_t>(HeldCount);

			// Single-octave order, as held-slot indices.
			uint8_t Slots[MaxHeld];
			for (int32_t I = 0; I < N; ++I) { Slots[I] = static_cast<uint8_t>(I); }

			// Insertion sort. AsPlayed orders by insertion counter; everything
			// else orders ascending by note number (Down / UpDown / DownUp /
			// Random derive their order from this via index math / random pick).
			for (int32_t I = 1; I < N; ++I)
			{
				const uint8_t Key = Slots[I];
				int32_t J = I - 1;
				while (J >= 0 && SortsAfter(Pat, Slots[J], Key))
				{
					Slots[J + 1] = Slots[J];
					--J;
				}
				Slots[J + 1] = Key;
			}

			const int32_t O = OctaveCount();
			ResolvedLen = N * O;
			for (int32_t Oct = 0; Oct < O; ++Oct)
			{
				for (int32_t I = 0; I < N; ++I)
				{
					const int32_t P = Oct * N + I;
					const uint8_t Slot = Slots[I];
					int32_t NoteVal = static_cast<int32_t>(Held[Slot].Note) + 12 * Oct;
					if (NoteVal > 127) { NoteVal = 127; }
					ExpNote[P] = static_cast<uint8_t>(NoteVal);
					ExpVel[P] = Held[Slot].Velocity;
				}
			}
		}

		// Insertion-sort comparator: true when slot A should come after slot B.
		bool SortsAfter(EPattern Pat, uint8_t A, uint8_t B) const
		{
			if (Pat == Pattern_AsPlayed)
			{
				return Held[A].Order > Held[B].Order;
			}
			if (Held[A].Note != Held[B].Note)
			{
				return Held[A].Note > Held[B].Note;
			}
			return Held[A].Order > Held[B].Order;  // stable tiebreak
		}

		int32_t PatternPeriod() const
		{
			if (ResolvedLen <= 0) { return 0; }
			const EPattern Pat = static_cast<EPattern>(Pattern.load(std::memory_order_relaxed));
			if (Pat == Pattern_UpDown || Pat == Pattern_DownUp)
			{
				return (ResolvedLen >= 2) ? (2 * ResolvedLen - 2) : 1;
			}
			return ResolvedLen;
		}

		// Map the current step position to an index into the expanded ascending
		// arrays. Reversal / turnaround is index math over that ascending order.
		int32_t PositionToExpandedIndex(int32_t Pos)
		{
			const int32_t L = ResolvedLen;
			const EPattern Pat = static_cast<EPattern>(Pattern.load(std::memory_order_relaxed));
			switch (Pat)
			{
				case Pattern_Up:
				case Pattern_AsPlayed:
					return Pos % L;
				case Pattern_Down:
					return L - 1 - (Pos % L);
				case Pattern_UpDown:
				{
					const int32_t K = (Pos < L) ? Pos : (2 * L - 2 - Pos);
					return K;
				}
				case Pattern_DownUp:
				{
					const int32_t K = (Pos < L) ? Pos : (2 * L - 2 - Pos);
					return L - 1 - K;
				}
				case Pattern_Random:
				{
					RandState = RandState * 1664525u + 1013904223u;
					return static_cast<int32_t>((RandState >> 16) % static_cast<uint32_t>(L));
				}
				default:
					return Pos % L;
			}
		}

		struct FHeldNote
		{
			uint8_t Note = 0;
			float Velocity = 0.0f;
			uint32_t Order = 0;
		};

		// Params (atomics — touched from the command queue).
		std::atomic<uint8_t> Pattern{ Pattern_Up };
		std::atomic<float>   Bpm{ 120.0f };
		std::atomic<uint8_t> RateChoice{ 3 };   // 1/16
		std::atomic<uint8_t> OctavesChoice{ 0 }; // 1 octave
		std::atomic<float>   GateFrac{ 0.5f };
		std::atomic<float>   Swing{ 0.0f };
		std::atomic<bool>    Latch{ false };

		// Held-note state (audio thread only).
		FHeldNote Held[MaxHeld]{};
		size_t HeldCount = 0;
		uint32_t InsertionCounter = 0;
		bool bPhysDown[128] = {};
		int32_t PhysicalKeysDown = 0;

		// Resolved sequence (rebuilt per advance).
		uint8_t ExpNote[MaxExpanded] = {};
		float ExpVel[MaxExpanded] = {};
		int32_t ResolvedLen = 0;

		// Playback state.
		double SampleRate = 48000.0;
		int32_t StepIndex = -1;
		uint64_t SamplesIntoStep = 0;
		double BaseSamplesPerStep = 24000.0;
		uint64_t SamplesPerStep = 24000;
		bool bPrevClockHigh = false;
		bool bPrevResetHigh = false;
		bool bPendingRetrigger = false;
		bool CurNoteValid = false;
		uint8_t CurNote = 69;
		float CurVel = 0.0f;
		uint8_t LastNote = 69;
		float LastFreq = 440.0f;
		uint32_t RandState = 0x9E3779B9u;
	};
}
