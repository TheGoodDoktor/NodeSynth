#pragma once

#include <cmath>
#include <cstdint>

namespace NodeSynth
{
	// One-pole low-pass smoother for parameter changes. The coefficient is derived
	// from a time constant (ms to reach ~63% of a step change) at a given sample rate.
	// Not thread-safe; intended to be owned by a single node and driven from Process.
	class FOnePoleSmoother
	{
	public:
		void Prepare(double SampleRate, float TimeConstantMs = 5.0f)
		{
			const double TimeConstantSamples = (TimeConstantMs * 0.001) * SampleRate;
			if (TimeConstantSamples <= 0.0)
			{
				Coefficient = 0.0f;
			}
			else
			{
				Coefficient = static_cast<float>(std::exp(-1.0 / TimeConstantSamples));
			}
		}

		void Reset(float Value)
		{
			Current = Value;
		}

		void SetTarget(float Value)
		{
			Target = Value;
		}

		float Tick()
		{
			Current = Target + (Current - Target) * Coefficient;
			return Current;
		}

		float GetCurrent() const
		{
			return Current;
		}

	private:
		float Coefficient = 0.0f;
		float Target = 0.0f;
		float Current = 0.0f;
	};
}
