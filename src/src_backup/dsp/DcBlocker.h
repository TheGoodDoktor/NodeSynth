#pragma once

#include <cmath>
#include <cstdint>

#include "dsp/Node.h"

namespace NodeSynth
{
	// Single-pole highpass at fixed 20 Hz. Useful after waveshapers and
	// other nonlinearities that can introduce a DC offset (a sustained
	// non-zero average) which manifests as a "clunk" at the start /
	// end of every note. No params — DC is a problem with one solution.
	//
	// Topology: y[n] = x[n] - x[n-1] + R * y[n-1].
	// R close to 1 gives a tighter cutoff at 20 Hz; here R = exp(-2π·fc/fs).
	class FDcBlocker : public TNodeBase<1, 1>
	{
	public:
		const char* GetTypeName() const override { return "DcBlocker"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return { { "Audio", EPortType::Audio, "Audio signal." } };
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Audio with DC offset removed (single-pole HP at 20 Hz)." } };
		}

		bool IsOutputStereo(uint32_t Index) const override { return Index == 0; }

		std::vector<FParamInfo> GetParamInfos() const override { return {}; }

		void Prepare(double InSampleRate) override
		{
			constexpr float CutoffHz = 20.0f;
			R = std::exp(-2.0f * 3.14159265358979f * CutoffHz / static_cast<float>(InSampleRate));
			XPrev[0] = XPrev[1] = 0.0f;
			YPrev[0] = YPrev[1] = 0.0f;
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* OutL = GetOutputBuffer(0, 0);
			float* OutR = GetOutputBuffer(0, 1);
			const float* InL = GetInputBuffer(0, 0);
			const float* InR = GetInputBuffer(0, 1);

			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				const float L = (InL != nullptr) ? InL[I] : 0.0f;
				const float R_in = (InR != nullptr) ? InR[I] : L;

				const float YL = L - XPrev[0] + R * YPrev[0];
				XPrev[0] = L;
				YPrev[0] = YL;
				OutL[I] = YL;

				const float YR = R_in - XPrev[1] + R * YPrev[1];
				XPrev[1] = R_in;
				YPrev[1] = YR;
				OutR[I] = YR;
			}
		}

	private:
		float R = 0.997f;          // pole radius — set in Prepare from sample rate
		float XPrev[2] = { 0.0f, 0.0f };
		float YPrev[2] = { 0.0f, 0.0f };
	};
}
