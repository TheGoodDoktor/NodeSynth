#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	// Feedback delay line with linear-interpolated fractional read tap and a
	// one-pole low-pass tone control on the feedback path. Two independent
	// lines (one per channel); both share the Time / Feedback / Tone params,
	// so a stereo input produces stereo output but a mono input produces
	// identical L and R until a future Stereo Spread param decorrelates them.
	// The delay buffers (each sized for 2 seconds of audio) are allocated once
	// in Prepare; Process performs no allocation. Modulating the delay time
	// via the Control input is intentionally NOT smoothed at the buffer level
	// — an LFO feeding the time port will modulate exactly as written. Slider
	// drags on the param are smoothed via FOnePoleSmoother to avoid zipper.
	class FDelay : public TNodeBase<2, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_TimeMs,
			Param_Feedback,
			Param_Tone,
			Param_COUNT,
		};

		enum EInput : uint32_t
		{
			Input_Audio,
			Input_TimeMs,  // Control: overrides the TimeMs param when connected
		};

		const char* GetTypeName() const override { return "Delay"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return
			{
				{ "Audio", EPortType::Audio,
					"Audio signal to delay." },
				{ "Time",  EPortType::Control,
					"Delay time in ms (overrides the Time param when connected)." },
			};
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Wet (delayed) signal, stereo. Mix with the dry input externally\n"
				"for a wet/dry blend." } };
		}

		bool IsOutputStereo(uint32_t Index) const override
		{
			return Index == 0;
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Time",     1.0f,  2000.0f, 250.0f, true,  EParamKind::Float, {},
					"Delay time in milliseconds. Smoothed when the Time Control input is disconnected." },
				{ "Feedback", 0.0f,  0.95f,   0.3f,   false, EParamKind::Float, {},
					"How much of the delayed signal feeds back into itself. Capped at 0.95 to prevent runaway." },
				{ "Tone",     0.05f, 1.0f,    1.0f,   false, EParamKind::Float, {},
					"High-frequency damping in the feedback path. 1.0 = bright (no damping), low values = dark / tape-style." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_TimeMs:   return TimeMs.load(std::memory_order_relaxed);
				case Param_Feedback: return Feedback.load(std::memory_order_relaxed);
				case Param_Tone:     return Tone.load(std::memory_order_relaxed);
				default:             return 0.0f;
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_TimeMs:
				{
					float V = Value;
					if (V < 1.0f) { V = 1.0f; }
					if (V > 2000.0f) { V = 2000.0f; }
					TimeMs.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_Feedback:
				{
					float V = Value;
					if (V < 0.0f) { V = 0.0f; }
					if (V > 0.95f) { V = 0.95f; }
					Feedback.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_Tone:
				{
					float V = Value;
					if (V < 0.05f) { V = 0.05f; }
					if (V > 1.0f) { V = 1.0f; }
					Tone.store(V, std::memory_order_relaxed);
					break;
				}
				default: break;
			}
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
			Capacity = static_cast<size_t>(InSampleRate * MaxDelaySeconds) + 4;  // +4 for interp safety
			Lines[0].Buffer.assign(Capacity, 0.0f);
			Lines[0].WriteIndex = 0;
			Lines[0].DamperState = 0.0f;
			Lines[1].Buffer.assign(Capacity, 0.0f);
			Lines[1].WriteIndex = 0;
			Lines[1].DamperState = 0.0f;

			TimeSmoother.Prepare(InSampleRate);
			TimeSmoother.Reset(TimeMs.load(std::memory_order_relaxed));
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* OutL = GetOutputBuffer(0, 0);
			float* OutR = GetOutputBuffer(0, 1);
			const float* AudioInL = GetInputBuffer(Input_Audio, 0);
			const float* AudioInR = GetInputBuffer(Input_Audio, 1);
			const float* TimeIn = GetInputBuffer(Input_TimeMs, 0);

			const float FeedbackNow = Feedback.load(std::memory_order_relaxed);
			const float ToneNow = Tone.load(std::memory_order_relaxed);
			TimeSmoother.SetTarget(TimeMs.load(std::memory_order_relaxed));

			const float MsToSamples = static_cast<float>(SampleRate * 0.001);
			const float MaxDelaySamples = static_cast<float>(Capacity - 2);

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				// Resolve the delay time. Control input overrides param when
				// connected (consistent with FOscillator's Freq/Amp inputs).
				float TimeMsValue;
				if (TimeIn != nullptr)
				{
					TimeMsValue = TimeIn[I];
					// Tick the smoother anyway so it tracks the param for when
					// the Control input goes away.
					TimeSmoother.Tick();
				}
				else
				{
					TimeMsValue = TimeSmoother.Tick();
				}

				float DelaySamples = TimeMsValue * MsToSamples;
				if (DelaySamples < 1.0f) { DelaySamples = 1.0f; }
				if (DelaySamples > MaxDelaySamples) { DelaySamples = MaxDelaySamples; }

				const int32_t Floor = static_cast<int32_t>(DelaySamples);
				const float Frac = DelaySamples - static_cast<float>(Floor);
				const int32_t Cap = static_cast<int32_t>(Capacity);

				const float DryInL = (AudioInL != nullptr) ? AudioInL[I] : 0.0f;
				const float DryInR = (AudioInR != nullptr) ? AudioInR[I] : DryInL;

				OutL[I] = ProcessLine(Lines[0], DryInL, Floor, Frac, Cap, FeedbackNow, ToneNow);
				OutR[I] = ProcessLine(Lines[1], DryInR, Floor, Frac, Cap, FeedbackNow, ToneNow);
			}
		}

		// Test / debug accessor — used to confirm the buffer doesn't reallocate
		// across Process calls. Returns the L line's buffer.
		const float* GetBufferData() const { return Lines[0].Buffer.data(); }
		size_t GetCapacity() const { return Capacity; }

	private:
		struct FLine
		{
			std::vector<float> Buffer;
			size_t WriteIndex = 0;
			float DamperState = 0.0f;
		};

		// Single-channel sample step. Pulled out of the per-sample loop so the
		// L and R lines run identical DSP without copy-paste.
		static float ProcessLine(FLine& Line, float DryIn, int32_t Floor, float Frac,
			int32_t Cap, float FeedbackNow, float ToneNow)
		{
			// Linear interpolation between the integer-Floor and Floor+1 taps.
			const int32_t IdxNewer = (static_cast<int32_t>(Line.WriteIndex) - Floor + Cap) % Cap;
			const int32_t IdxOlder = (static_cast<int32_t>(Line.WriteIndex) - Floor - 1 + Cap) % Cap;
			const float Newer = Line.Buffer[static_cast<size_t>(IdxNewer)];
			const float Older = Line.Buffer[static_cast<size_t>(IdxOlder)];
			const float Delayed = (1.0f - Frac) * Newer + Frac * Older;

			// One-pole low-pass on the feedback path. ToneNow ∈ [0.05, 1].
			Line.DamperState += ToneNow * (Delayed - Line.DamperState);

			Line.Buffer[Line.WriteIndex] = DryIn + FeedbackNow * Line.DamperState;
			++Line.WriteIndex;
			if (Line.WriteIndex >= Line.Buffer.size())
			{
				Line.WriteIndex = 0;
			}
			return Delayed;
		}

		static constexpr double MaxDelaySeconds = 2.0;

		std::atomic<float> TimeMs{ 250.0f };
		std::atomic<float> Feedback{ 0.3f };
		std::atomic<float> Tone{ 1.0f };

		double SampleRate = 48000.0;
		size_t Capacity = 0;
		FLine Lines[2];
		FOnePoleSmoother TimeSmoother;
	};
}
