#pragma once

#include <atomic>
#include <cstdint>

#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	// Mid-side stereo widener. Single Width param. Width=1 → bit-identical
	// passthrough. Width=0 → mono (L = R = mid). Width>1 → exaggerated
	// stereo (loud side content; risk of mono cancellation if pushed too far).
	class FStereoWidener : public TNodeBase<1, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Width,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "StereoWidener"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Audio", EPortType::Audio, "Stereo signal to widen / narrow." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Width-adjusted stereo. Width=1 → bit-identical; 0 → mono;\n"
				">1 → exaggerated side content (mind mono compatibility)." } };
		}

		bool IsOutputStereo(uint32_t Index) const override { return Index == 0; }

		std::vector<FParamInfo> GetParamInfos() const override
		{
			return
			{
				{ "Width", 0.0f, 2.0f, 1.0f, false, EParamKind::Float, {},
					"Side-content scale. 0 = mono, 1 = passthrough, 2 = double-wide.\n"
					"Above 1 risks anti-phase cancellation when summed to mono." },
			};
		}

		float GetParamValue(uint32_t Index) const override
		{
			return (Index == Param_Width) ? Width.load(std::memory_order_relaxed) : 0.0f;
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			if (Index != Param_Width) { return; }
			float V = Value;
			if (V < 0.0f) { V = 0.0f; }
			if (V > 2.0f) { V = 2.0f; }
			Width.store(V, std::memory_order_relaxed);
		}

		void Prepare(double InSampleRate) override
		{
			WidthSmoother.Prepare(InSampleRate, 30.0f);
			WidthSmoother.Reset(Width.load(std::memory_order_relaxed));
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* OutL = GetOutputBuffer(0, 0);
			float* OutR = GetOutputBuffer(0, 1);
			const float* InL = GetInputBuffer(0, 0);
			const float* InR = GetInputBuffer(0, 1);

			WidthSmoother.SetTarget(Width.load(std::memory_order_relaxed));

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float L = (InL != nullptr) ? InL[I] : 0.0f;
				const float R = (InR != nullptr) ? InR[I] : L;
				const float Mid = (L + R) * 0.5f;
				const float Side = (L - R) * 0.5f;
				const float W = WidthSmoother.Tick();
				OutL[I] = Mid + W * Side;
				OutR[I] = Mid - W * Side;
			}
		}

	private:
		std::atomic<float> Width{ 1.0f };
		FOnePoleSmoother WidthSmoother;
	};
}
