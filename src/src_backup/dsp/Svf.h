#pragma once

#include <atomic>
#include <cmath>
#include <numbers>

#include "dsp/Node.h"

namespace NodeSynth
{
	// Linear trapezoidal state-variable filter (Zavalishin / Simper TPT form).
	// Stable across the full cutoff × resonance range including self-oscillation.
	//
	// Three inputs: Audio, Cutoff (Hz), Resonance (0..1). Three outputs: LP, HP, BP.
	// Disconnected Cutoff/Resonance inputs fall back to the param slider value.
	class FSvf : public TNodeBase<3, 3>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Cutoff,
			Param_Resonance,
			Param_COUNT,
		};

		enum EInput : uint32_t
		{
			Input_Audio,
			Input_Cutoff,
			Input_Resonance,
		};

		enum EOutput : uint32_t
		{
			Output_LowPass,
			Output_HighPass,
			Output_BandPass,
		};

		const char* GetTypeName() const override { return "SVF"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return
			{
				{ "Audio",     EPortType::Audio,
					"Audio signal to filter." },
				{ "Cutoff",    EPortType::Control,
					"Cutoff frequency in Hz. Overrides the Cutoff param when connected." },
				{ "Resonance", EPortType::Control,
					"Resonance / Q (0..1). Overrides the Resonance param when connected." },
			};
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return
			{
				{ "LP", EPortType::Audio,
					"Low-pass output. Frequencies below Cutoff pass through; higher frequencies attenuate." },
				{ "HP", EPortType::Audio,
					"High-pass output. Frequencies above Cutoff pass through; lower frequencies attenuate." },
				{ "BP", EPortType::Audio,
					"Band-pass output. Only frequencies near Cutoff pass; the band width tightens with Resonance." },
			};
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Cutoff",    20.0f, 20000.0f, 1000.0f, true,  EParamKind::Float, {},
					"Filter cutoff frequency in Hz. Clamped to a safe range every sample.",
					/* bHidden */ false, /* ControlInputIndex */ Input_Cutoff },
				{ "Resonance", 0.0f,  1.0f,     0.2f,   false, EParamKind::Float, {},
					"Resonance / Q. 1.0 self-oscillates cleanly.",
					/* bHidden */ false, /* ControlInputIndex */ Input_Resonance },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_Cutoff:    return Cutoff.load(std::memory_order_relaxed);
				case Param_Resonance: return Resonance.load(std::memory_order_relaxed);
				default: return 0.0f;
			}
		}

		float GetLiveParamValue(uint32_t Index) const override
		{
			if (Index != Param_Cutoff && Index != Param_Resonance)
			{
				return GetParamValue(Index);
			}
			// Activity proxy is the per-voice audio level — pick the voice
			// receiving audio. Mono master writes to slot 0.
			int32_t Best = 0;
			float BestLevel = LastAudioLevelPerVoice[0].load(std::memory_order_relaxed);
			for (int32_t V = 1; V < LiveMaxVoices; ++V)
			{
				const float L = LastAudioLevelPerVoice[V].load(std::memory_order_relaxed);
				if (L > BestLevel) { BestLevel = L; Best = V; }
			}
			if (BestLevel <= 1e-4f) { return GetParamValue(Index); }
			return (Index == Param_Cutoff)
				? LastCutoffPerVoice[Best].load(std::memory_order_relaxed)
				: LastResonancePerVoice[Best].load(std::memory_order_relaxed);
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_Cutoff:    Cutoff.store(Value, std::memory_order_relaxed); break;
				case Param_Resonance: Resonance.store(Value, std::memory_order_relaxed); break;
				default: break;
			}
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
			Ic1Eq = 0.0f;
			Ic2Eq = 0.0f;
		}

		void Process(const FProcessContext& Ctx) override
		{
			const float* Audio = GetInputBuffer(Input_Audio);
			const float* CutoffBuf = GetInputBuffer(Input_Cutoff);
			const float* ResBuf = GetInputBuffer(Input_Resonance);

			float* LP = GetOutputBuffer(Output_LowPass);
			float* HP = GetOutputBuffer(Output_HighPass);
			float* BP = GetOutputBuffer(Output_BandPass);

			if (Audio == nullptr)
			{
				for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
				{
					LP[I] = 0.0f;
					HP[I] = 0.0f;
					BP[I] = 0.0f;
				}
				return;
			}

			const float CutoffParam = Cutoff.load(std::memory_order_relaxed);
			const float ResParam = Resonance.load(std::memory_order_relaxed);
			const float CutoffMax = static_cast<float>(0.49 * SampleRate);
			float MaxAbsAudio = 0.0f;  // activity proxy for the live-mirror gate

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				float CutoffHz = (CutoffBuf != nullptr) ? CutoffBuf[I] : CutoffParam;
				if (CutoffHz < 20.0f)
				{
					CutoffHz = 20.0f;
				}
				if (CutoffHz > CutoffMax)
				{
					CutoffHz = CutoffMax;
				}
				float Res = (ResBuf != nullptr) ? ResBuf[I] : ResParam;
				if (Res < 0.0f)
				{
					Res = 0.0f;
				}
				if (Res > 1.0f)
				{
					Res = 1.0f;
				}
				const float AbsIn = std::fabs(Audio[I]);
				if (AbsIn > MaxAbsAudio) { MaxAbsAudio = AbsIn; }

				// TPT coefficients: g = tan(pi * fc / fs), k = 2 * (1 - res) gives
				// k=2 at res=0 (heavy damping), k=0 at res=1 (self-oscillation).
				const float G = static_cast<float>(std::tan(std::numbers::pi * CutoffHz / SampleRate));
				const float K = 2.0f * (1.0f - Res);
				const float A1 = 1.0f / (1.0f + G * (G + K));
				const float A2 = G * A1;
				const float A3 = G * A2;

				const float V0 = Audio[I];
				const float V3 = V0 - Ic2Eq;
				const float V1 = A1 * Ic1Eq + A2 * V3;
				const float V2 = Ic2Eq + A2 * Ic1Eq + A3 * V3;
				Ic1Eq = 2.0f * V1 - Ic1Eq;
				Ic2Eq = 2.0f * V2 - Ic2Eq;

				LP[I] = V2;
				BP[I] = V1;
				HP[I] = V0 - K * V1 - V2;
			}

			// Latch the last sample's effective Cutoff / Resonance for the
			// property panel's live readout. Per-voice clones mirror to
			// MasterMirror so the UI's master-held node updates too.
			const uint32_t Last = (Ctx.BlockSize > 0) ? Ctx.BlockSize - 1u : 0u;
			const float LiveCutoff = (CutoffBuf != nullptr) ? CutoffBuf[Last] : CutoffParam;
			const float LiveRes    = (ResBuf    != nullptr) ? ResBuf[Last]    : ResParam;
			// Each clone writes to its own VoiceIndex slot of the master's
			// per-voice arrays. The UI's GetLiveParamValue picks the voice
			// with the highest audio level. No race.
			auto* Target = (MasterMirror != nullptr)
				? static_cast<FSvf*>(MasterMirror) : this;
			const int32_t Slot = (VoiceIndex >= 0 && VoiceIndex < LiveMaxVoices)
				? VoiceIndex : 0;
			Target->LastCutoffPerVoice[Slot].store(LiveCutoff, std::memory_order_relaxed);
			Target->LastResonancePerVoice[Slot].store(LiveRes, std::memory_order_relaxed);
			Target->LastAudioLevelPerVoice[Slot].store(MaxAbsAudio, std::memory_order_relaxed);
		}

	private:
		std::atomic<float> Cutoff{ 1000.0f };
		std::atomic<float> Resonance{ 0.2f };
		// Per-voice live values. Each clone writes its own slot;
		// GetLiveParamValue picks the voice with the highest audio level.
		std::atomic<float> LastCutoffPerVoice[LiveMaxVoices] = {};
		std::atomic<float> LastResonancePerVoice[LiveMaxVoices] = {};
		std::atomic<float> LastAudioLevelPerVoice[LiveMaxVoices] = {};
		double SampleRate = 48000.0;
		float Ic1Eq = 0.0f;
		float Ic2Eq = 0.0f;
	};
}
