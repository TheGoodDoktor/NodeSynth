#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dsp/Node.h"
#include "dsp/Wavetable.h"
#include "io/WavetableBrowser.h"

namespace NodeSynth
{
	// Wavetable oscillator. Plays back a stack of single-cycle waveforms;
	// the Position param (or its Control input) selects between adjacent
	// frames via linear interpolation. v1: no anti-aliasing (always reads
	// the source frames). WT.3 will add mip-mapped AA. See
	// docs/PLAN-WAVETABLES.md.
	//
	// Threading: the WAV is parsed on the UI thread inside SetParamString,
	// then atomic-swapped into CurrentTable. The audio thread loads the
	// shared_ptr once per Process call and reads through it.
	class FWavetableOscillator : public TNodeBase<3, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Wavetable,   // String — absolute path to a .wav (UI fills this)
			Param_Frequency,   // Hz, used when Input_Frequency is unwired
			Param_Position,    // 0..1 — frame index, modulatable via Input_Position
			Param_Detune,      // -100..+100 cents
			Param_Phase,       // 0..1 initial phase offset
			Param_Amplitude,   // 0..1, modulatable via Input_Amplitude
			Param_COUNT,
		};

		enum EInput : uint32_t
		{
			Input_Frequency,
			Input_Position,
			Input_Amplitude,
		};

		const char* GetTypeName() const override { return "WavetableOscillator"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return
			{
				{ "Freq", EPortType::Control,
					"Frequency in Hz. Overrides the Frequency param when connected." },
				{ "Pos",  EPortType::Control,
					"Frame position 0..1. Overrides the Position param when connected.\n"
					"Wire an ADSR or LFO here for the classic wavetable morph." },
				{ "Amp",  EPortType::Control,
					"Amplitude 0..1. Overrides the Amplitude param when connected." },
			};
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Audio output sampled from the loaded wavetable." } };
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Wavetable", 0.0f, 0.0f, 0.0f, false, EParamKind::String, {},
					"Path to a .wav (relative to wavetables/ or absolute). Length\n"
					"must be a multiple of 2048 samples. The custom UI panel\n"
					"surfaces a dropdown of bundled wavetables.",
					/* bHidden */ true },
				{ "Frequency", 20.0f, 20000.0f, 440.0f, true, EParamKind::Float, {},
					"Pitch in Hz. The Freq Control input overrides this when connected.",
					/* bHidden */ false, /* ControlInputIndex */ Input_Frequency },
				{ "Position",  0.0f, 1.0f, 0.0f, false, EParamKind::Float, {},
					"Frame index normalised to 0..1. The Pos Control input overrides\n"
					"this when connected.",
					/* bHidden */ false, /* ControlInputIndex */ Input_Position },
				{ "Detune",  -100.0f, 100.0f, 0.0f, false, EParamKind::Float, {},
					"Pitch trim in cents." },
				{ "Phase",     0.0f, 1.0f, 0.0f, false, EParamKind::Float, {},
					"Initial phase offset 0..1. Applied at Prepare()." },
				{ "Amplitude", 0.0f, 1.0f, 0.5f, false, EParamKind::Float, {},
					"Output level 0..1. The Amp Control input overrides this when connected.",
					/* bHidden */ false, /* ControlInputIndex */ Input_Amplitude },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_Frequency: return Frequency.load(std::memory_order_relaxed);
				case Param_Position:  return Position.load(std::memory_order_relaxed);
				case Param_Detune:    return Detune.load(std::memory_order_relaxed);
				case Param_Phase:     return PhaseOffset.load(std::memory_order_relaxed);
				case Param_Amplitude: return Amplitude.load(std::memory_order_relaxed);
				default:              return 0.0f;
			}
		}

		float GetLiveParamValue(uint32_t Index) const override
		{
			if (Index != Param_Frequency && Index != Param_Position
				&& Index != Param_Amplitude)
			{
				return GetParamValue(Index);
			}
			int32_t Best = 0;
			float BestAmp = LastAmplitudePerVoice[0].load(std::memory_order_relaxed);
			for (int32_t V = 1; V < LiveMaxVoices; ++V)
			{
				const float A = LastAmplitudePerVoice[V].load(std::memory_order_relaxed);
				if (A > BestAmp) { BestAmp = A; Best = V; }
			}
			if (BestAmp <= 1e-3f) { return GetParamValue(Index); }
			switch (Index)
			{
				case Param_Frequency: return LastFrequencyPerVoice[Best].load(std::memory_order_relaxed);
				case Param_Position:  return LastPositionPerVoice[Best].load(std::memory_order_relaxed);
				case Param_Amplitude: return BestAmp;
				default:              return GetParamValue(Index);
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_Frequency:
				{
					float V = Value;
					if (V < 20.0f)    { V = 20.0f; }
					if (V > 20000.0f) { V = 20000.0f; }
					Frequency.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_Position:
				{
					float V = Value;
					if (V < 0.0f) { V = 0.0f; }
					if (V > 1.0f) { V = 1.0f; }
					Position.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_Detune:
				{
					float V = Value;
					if (V < -100.0f) { V = -100.0f; }
					if (V > 100.0f)  { V = 100.0f; }
					Detune.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_Phase:
				{
					float V = Value;
					if (V < 0.0f) { V = 0.0f; }
					if (V > 1.0f) { V = 1.0f; }
					PhaseOffset.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_Amplitude:
				{
					float V = Value;
					if (V < 0.0f) { V = 0.0f; }
					if (V > 1.0f) { V = 1.0f; }
					Amplitude.store(V, std::memory_order_relaxed);
					break;
				}
				default: break;
			}
		}

		std::string GetParamString(uint32_t Index) const override
		{
			return (Index == Param_Wavetable) ? WavetablePath : std::string{};
		}

		void SetParamString(uint32_t Index, const std::string& Value) override
		{
			if (Index != Param_Wavetable) { return; }
			WavetablePath = Value;
			if (Value.empty())
			{
				std::atomic_store_explicit(&CurrentTable,
					std::shared_ptr<FWavetableData>{},
					std::memory_order_release);
				return;
			}
			// Try to resolve as relative-to-bundled / user dir first; fall
			// back to the path as-given so an absolute path still works
			// the same way it did before WT.5.
			std::filesystem::path Resolved = ResolveWavetablePath(Value);
			if (Resolved.empty()) { Resolved = Value; }
			std::shared_ptr<FWavetableData> Loaded = LoadWavetable(Resolved);
			// Even on failure we update the path string so the UI shows what
			// the user typed. The audio thread sees nullptr and outputs silence.
			std::atomic_store_explicit(&CurrentTable, std::move(Loaded),
				std::memory_order_release);
		}

		// UI accessor: the current loaded wavetable, or nullptr if none.
		// Returned shared_ptr keeps the data alive for the caller's scope.
		std::shared_ptr<FWavetableData> GetCurrentTable() const
		{
			return std::atomic_load_explicit(&CurrentTable, std::memory_order_acquire);
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
			Phase = static_cast<double>(PhaseOffset.load(std::memory_order_relaxed));
			if (Phase < 0.0) { Phase = 0.0; }
			if (Phase >= 1.0) { Phase = 0.0; }
			LastMipIndex = -1;  // disable crossfade on the first block
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Out = GetOutputBuffer(0);
			if (Out == nullptr) { return; }

			const std::shared_ptr<FWavetableData> Table =
				std::atomic_load_explicit(&CurrentTable, std::memory_order_acquire);
			if (!Table || Table->Frames.empty())
			{
				for (uint32_t I = 0; I < Ctx.BlockSize; ++I) { Out[I] = 0.0f; }
				return;
			}

			const float* FreqIn = GetInputBuffer(Input_Frequency);
			const float* PosIn  = GetInputBuffer(Input_Position);
			const float* AmpIn  = GetInputBuffer(Input_Amplitude);

			const float DetuneCents = Detune.load(std::memory_order_relaxed);
			const float DetuneRatio = std::pow(2.0f, DetuneCents / 1200.0f);
			const float FreqParam = Frequency.load(std::memory_order_relaxed);
			const float PosParam = Position.load(std::memory_order_relaxed);
			const float AmpParam = Amplitude.load(std::memory_order_relaxed);

			const uint32_t NumFrames = Table->NumFrames();
			constexpr uint32_t FrameSize = FWavetableData::FrameSize;
			const float MaxFrameIndex =
				(NumFrames > 1) ? static_cast<float>(NumFrames - 1) : 0.0f;
			const float NyqLimit = static_cast<float>(0.49 * SampleRate);

			// Mip selection: pick the lowest mip whose harmonic count
			// keeps NumHarmonics * fundamental <= Nyquist. We compute it
			// from the fundamental at sample 0 (block-rate selection —
			// per-sample switching would be prohibitive and unnecessary).
			const float FreqAt0 =
				((FreqIn != nullptr) ? FreqIn[0] : FreqParam) * DetuneRatio;
			const int32_t MipNow = SelectMip(FreqAt0);

			// Crossfade across the block when the mip index changes,
			// otherwise read from a single mip the whole way through.
			const bool bCrossfade = (LastMipIndex >= 0 && LastMipIndex != MipNow);
			const int32_t MipPrev = bCrossfade ? LastMipIndex : MipNow;

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				// Frequency: Control input overrides the param when wired.
				const float FreqRaw = (FreqIn != nullptr) ? FreqIn[I] : FreqParam;
				float Freq = FreqRaw * DetuneRatio;
				if (Freq < 0.0f)    { Freq = 0.0f; }
				if (Freq > NyqLimit) { Freq = NyqLimit; }

				const float PosRaw = (PosIn != nullptr) ? PosIn[I] : PosParam;
				const float ClampedPos =
					(PosRaw < 0.0f) ? 0.0f : (PosRaw > 1.0f ? 1.0f : PosRaw);
				const float FrameF = ClampedPos * MaxFrameIndex;
				const uint32_t FrameA = static_cast<uint32_t>(std::floor(FrameF));
				const uint32_t FrameB = (FrameA + 1u < NumFrames)
					? FrameA + 1u : FrameA;
				const float FrameAlpha = FrameF - static_cast<float>(FrameA);

				const float SampleIdxF = static_cast<float>(Phase * FrameSize);
				const uint32_t IdxA = static_cast<uint32_t>(std::floor(SampleIdxF))
					% FrameSize;
				const uint32_t IdxB = (IdxA + 1u) % FrameSize;
				const float SampleAlpha = SampleIdxF - std::floor(SampleIdxF);

				const float SampleNew = SampleAt(*Table, FrameA, FrameB, FrameAlpha,
					IdxA, IdxB, SampleAlpha, MipNow);
				float Sample = SampleNew;
				if (bCrossfade)
				{
					const float SamplePrev = SampleAt(*Table, FrameA, FrameB, FrameAlpha,
						IdxA, IdxB, SampleAlpha, MipPrev);
					const float Alpha = (Ctx.BlockSize > 1)
						? static_cast<float>(I) / static_cast<float>(Ctx.BlockSize - 1)
						: 1.0f;
					Sample = SamplePrev * (1.0f - Alpha) + SampleNew * Alpha;
				}

				const float Amp = (AmpIn != nullptr) ? AmpIn[I] : AmpParam;
				Out[I] = Sample * Amp;

				Phase += static_cast<double>(Freq) / SampleRate;
				if (Phase >= 1.0) { Phase -= std::floor(Phase); }
			}
			LastMipIndex = MipNow;

			// Latch the last sample's effective Frequency / Position /
			// Amplitude for the property panel's live readout. Per-voice
			// clones mirror to MasterMirror since the UI reads the master
			// node, which never has Process called on it.
			const uint32_t Last = (Ctx.BlockSize > 0) ? Ctx.BlockSize - 1u : 0u;
			const float LiveFreq =
				((FreqIn != nullptr) ? FreqIn[Last] : FreqParam) * DetuneRatio;
			const float LivePos = (PosIn != nullptr) ? PosIn[Last] : PosParam;
			const float LiveAmp = (AmpIn != nullptr) ? AmpIn[Last] : AmpParam;
			// Each clone writes its own VoiceIndex slot of the master's
			// per-voice arrays. UI scans for loudest voice — no race.
			auto* Target = (MasterMirror != nullptr)
				? static_cast<FWavetableOscillator*>(MasterMirror) : this;
			const int32_t Slot = (VoiceIndex >= 0 && VoiceIndex < LiveMaxVoices)
				? VoiceIndex : 0;
			Target->LastFrequencyPerVoice[Slot].store(LiveFreq, std::memory_order_relaxed);
			Target->LastPositionPerVoice[Slot].store(LivePos, std::memory_order_relaxed);
			Target->LastAmplitudePerVoice[Slot].store(LiveAmp, std::memory_order_relaxed);
		}

		// Per-voice cloning: share the loaded wavetable pointer across
		// clones rather than re-parsing the WAV per voice. Default Clone
		// would call SetParamString → LoadWavetable on every voice, which
		// would re-read the file from disk eight times.
		std::shared_ptr<INode> Clone() const override
		{
			auto C = std::make_shared<FWavetableOscillator>();
			C->WavetablePath = WavetablePath;
			std::atomic_store_explicit(&C->CurrentTable,
				std::atomic_load_explicit(&CurrentTable, std::memory_order_acquire),
				std::memory_order_release);
			C->Frequency.store(Frequency.load(std::memory_order_relaxed));
			C->Position.store(Position.load(std::memory_order_relaxed));
			C->Detune.store(Detune.load(std::memory_order_relaxed));
			C->PhaseOffset.store(PhaseOffset.load(std::memory_order_relaxed));
			C->Amplitude.store(Amplitude.load(std::memory_order_relaxed));
			// See NodeRegistry's default Clone — clones mirror Last* atomics
			// back to the master so per-voice nodes' UI live readout works.
			C->MasterMirror = const_cast<FWavetableOscillator*>(this);
			return C;
		}

	private:
		// Read one sample at the given frame pair / sample pair / mip,
		// with linear interpolation in both the sample and frame
		// dimensions. Pulled out so the crossfade-on-mip-change path
		// can read both the old and new mip's value.
		static float SampleAt(const FWavetableData& Table,
			uint32_t FrameA, uint32_t FrameB, float FrameAlpha,
			uint32_t IdxA, uint32_t IdxB, float SampleAlpha, int32_t Mip)
		{
			const std::vector<float>& A = Table.Frames[FrameA].Mips[static_cast<size_t>(Mip)];
			const std::vector<float>& B = Table.Frames[FrameB].Mips[static_cast<size_t>(Mip)];
			const float SampleA = A[IdxA] * (1.0f - SampleAlpha) + A[IdxB] * SampleAlpha;
			const float SampleB = B[IdxA] * (1.0f - SampleAlpha) + B[IdxB] * SampleAlpha;
			return SampleA * (1.0f - FrameAlpha) + SampleB * FrameAlpha;
		}

		// Pick a mip level for the given playback frequency. Mip M has at
		// most NumHarmonics(M) = 1024 / 2^M non-zero spectral bins; we
		// want NumHarmonics(M) * Freq <= Nyquist, i.e.
		//   2^M >= 2 * FrameSize * Freq / SampleRate.
		// Solving: M >= log2(2 * FrameSize * Freq / SampleRate). The
		// ceil-log2 is implemented with a bit-scan on the integer
		// rather than a float log to keep it cheap.
		int32_t SelectMip(float FreqHz) const
		{
			if (FreqHz <= 0.0f) { return 0; }
			const float Threshold =
				static_cast<float>(FWavetableData::FrameSize) * FreqHz
				/ static_cast<float>(SampleRate);
			int32_t Mip = 0;
			float Pow2 = 1.0f;
			while (Pow2 < Threshold && Mip < static_cast<int32_t>(FWavetableFrame::NumMips) - 1)
			{
				Pow2 *= 2.0f;
				++Mip;
			}
			return Mip;
		}

		std::shared_ptr<FWavetableData> CurrentTable{ nullptr };
		std::string WavetablePath;

		std::atomic<float> Frequency{ 440.0f };
		std::atomic<float> Position{ 0.0f };
		std::atomic<float> Detune{ 0.0f };
		std::atomic<float> PhaseOffset{ 0.0f };
		std::atomic<float> Amplitude{ 0.5f };
		// Per-voice live values. Each clone writes its own slot; the master's
		// GetLiveParamValue scans for the loudest voice (Amplitude proxy)
		// and reads from there. Mono nodes use slot 0.
		std::atomic<float> LastFrequencyPerVoice[LiveMaxVoices] = {};
		std::atomic<float> LastPositionPerVoice[LiveMaxVoices] = {};
		std::atomic<float> LastAmplitudePerVoice[LiveMaxVoices] = {};

		double Phase = 0.0;
		double SampleRate = 48000.0;
		// Mip used during the previous block, for the one-block crossfade
		// when the block-rate mip selection changes. -1 = first block,
		// suppress crossfade.
		int32_t LastMipIndex = -1;
	};
}
