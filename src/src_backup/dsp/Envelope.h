#pragma once

#include <cmath>
#include <cstdint>

namespace NodeSynth
{
	// Peak-following one-pole envelope detector. Tracks the envelope of |x|
	// with asymmetric attack / release coefficients — fast attack catches
	// transients, slow release lets the envelope decay smoothly.
	//
	// Used by FCompressor, FLimiter, FGate. Coefficients are recomputed
	// once per block (call SetTimes after Prepare). Process() is one
	// branch + three multiplies + one add per sample.
	struct FEnvelopeFollower
	{
		void Prepare(double InSampleRate)
		{
			SampleRate = InSampleRate;
			State = 0.0f;
			AttackCoeff = 0.0f;
			ReleaseCoeff = 0.0f;
		}

		void Reset() { State = 0.0f; }

		// Set the attack / release time constants. Call once per block
		// from the host node's Process — recomputing per-sample would
		// cost an exp() per cycle, which is overkill for params that
		// only change at slider rate.
		void SetTimes(float AttackMs, float ReleaseMs)
		{
			AttackCoeff = TimeMsToCoeff(AttackMs);
			ReleaseCoeff = TimeMsToCoeff(ReleaseMs);
		}

		// One-sample peak-follower step. Asymmetric: attack coefficient
		// when the new sample is above the current envelope, release
		// when below. Standard peak-detector topology.
		float Process(float Input)
		{
			const float Abs = std::fabs(Input);
			const float Coeff = (Abs > State) ? AttackCoeff : ReleaseCoeff;
			State = Coeff * State + (1.0f - Coeff) * Abs;
			return State;
		}

		float GetCurrent() const { return State; }

	private:
		float TimeMsToCoeff(float TimeMs) const
		{
			if (TimeMs <= 0.0f) { return 0.0f; }
			const double Tau = SampleRate * static_cast<double>(TimeMs) * 0.001;
			return static_cast<float>(std::exp(-1.0 / Tau));
		}

		double SampleRate = 48000.0;
		float State = 0.0f;
		float AttackCoeff = 0.0f;
		float ReleaseCoeff = 0.0f;
	};
}
