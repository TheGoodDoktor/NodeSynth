#pragma once

#include <cmath>
#include <cstdint>

namespace NodeSynth
{
	// Direct-form II transposed biquad coefficients. Coefficient computation
	// follows the RBJ "Cookbook formulae for audio EQ biquad filter
	// coefficients" — the standard reference. Compute once per block, apply
	// per sample via FBiquadState.
	//
	// Splitting state from coeffs lets a stereo node share one FBiquadCoeffs
	// across two FBiquadState instances (one per channel), saving the per-
	// param-change coefficient calc on the second channel.
	struct FBiquadCoeffs
	{
		float B0 = 1.0f, B1 = 0.0f, B2 = 0.0f;
		float A1 = 0.0f, A2 = 0.0f;

		void SetLowShelf(float Fc, float Q, float DbGain, float SampleRate)
		{
			ComputeShelf(Fc, Q, DbGain, SampleRate, /*bLow*/ true);
		}

		void SetHighShelf(float Fc, float Q, float DbGain, float SampleRate)
		{
			ComputeShelf(Fc, Q, DbGain, SampleRate, /*bLow*/ false);
		}

		void SetPeak(float Fc, float Q, float DbGain, float SampleRate)
		{
			const float A = std::pow(10.0f, DbGain / 40.0f);
			const float W0 = 2.0f * 3.14159265358979f * Fc / SampleRate;
			const float CosW = std::cos(W0);
			const float Alpha = std::sin(W0) / (2.0f * Q);

			const float B0_ = 1.0f + Alpha * A;
			const float B1_ = -2.0f * CosW;
			const float B2_ = 1.0f - Alpha * A;
			const float A0_ = 1.0f + Alpha / A;
			const float A1_ = -2.0f * CosW;
			const float A2_ = 1.0f - Alpha / A;
			Normalise(B0_, B1_, B2_, A0_, A1_, A2_);
		}

		void SetIdentity()
		{
			B0 = 1.0f; B1 = 0.0f; B2 = 0.0f;
			A1 = 0.0f; A2 = 0.0f;
		}

	private:
		void ComputeShelf(float Fc, float Q, float DbGain, float SampleRate, bool bLow)
		{
			const float A = std::pow(10.0f, DbGain / 40.0f);
			const float W0 = 2.0f * 3.14159265358979f * Fc / SampleRate;
			const float CosW = std::cos(W0);
			const float Alpha = std::sin(W0) / (2.0f * Q);
			const float Beta = 2.0f * std::sqrt(A) * Alpha;

			float B0_, B1_, B2_, A0_, A1_, A2_;
			if (bLow)
			{
				B0_ = A * ((A + 1.0f) - (A - 1.0f) * CosW + Beta);
				B1_ = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * CosW);
				B2_ = A * ((A + 1.0f) - (A - 1.0f) * CosW - Beta);
				A0_ = (A + 1.0f) + (A - 1.0f) * CosW + Beta;
				A1_ = -2.0f * ((A - 1.0f) + (A + 1.0f) * CosW);
				A2_ = (A + 1.0f) + (A - 1.0f) * CosW - Beta;
			}
			else
			{
				B0_ = A * ((A + 1.0f) + (A - 1.0f) * CosW + Beta);
				B1_ = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * CosW);
				B2_ = A * ((A + 1.0f) + (A - 1.0f) * CosW - Beta);
				A0_ = (A + 1.0f) - (A - 1.0f) * CosW + Beta;
				A1_ = 2.0f * ((A - 1.0f) - (A + 1.0f) * CosW);
				A2_ = (A + 1.0f) - (A - 1.0f) * CosW - Beta;
			}
			Normalise(B0_, B1_, B2_, A0_, A1_, A2_);
		}

		void Normalise(float B0_, float B1_, float B2_, float A0_, float A1_, float A2_)
		{
			const float Inv = (A0_ != 0.0f) ? (1.0f / A0_) : 1.0f;
			B0 = B0_ * Inv;
			B1 = B1_ * Inv;
			B2 = B2_ * Inv;
			A1 = A1_ * Inv;
			A2 = A2_ * Inv;
		}
	};

	// Per-channel state for one biquad filter. Direct-form I —
	// straightforward to verify, ~5 mul + 4 add per sample. (DF2T would
	// be slightly cheaper but is easier to get wrong via sign confusion.)
	struct FBiquadState
	{
		float X1 = 0.0f, X2 = 0.0f;
		float Y1 = 0.0f, Y2 = 0.0f;

		void Reset()
		{
			X1 = 0.0f; X2 = 0.0f;
			Y1 = 0.0f; Y2 = 0.0f;
		}

		float Process(const FBiquadCoeffs& C, float X)
		{
			const float Y = C.B0 * X + C.B1 * X1 + C.B2 * X2 - C.A1 * Y1 - C.A2 * Y2;
			X2 = X1; X1 = X;
			Y2 = Y1; Y1 = Y;
			return Y;
		}
	};
}
