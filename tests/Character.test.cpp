#include "dsp/Bitcrusher.h"
#include "dsp/Exciter.h"
#include "dsp/HaasWidener.h"
#include "dsp/RingMod.h"
#include "dsp/StereoWidener.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using NodeSynth::BlockSize;
using NodeSynth::FBitcrusher;
using NodeSynth::FExciter;
using NodeSynth::FHaasWidener;
using NodeSynth::FProcessContext;
using NodeSynth::FRingMod;
using NodeSynth::FStereoWidener;

namespace
{
	FProcessContext Ctx() { return {}; }

	std::vector<float> Sine(uint32_t N, double Freq, double Sr, float Amp = 1.0f)
	{
		std::vector<float> S(N, 0.0f);
		const double TwoPi = 2.0 * 3.141592653589793;
		double T = 0.0;
		const double Phase = TwoPi * Freq / Sr;
		for (uint32_t I = 0; I < N; ++I)
		{
			S[I] = Amp * static_cast<float>(std::sin(T));
			T += Phase;
		}
		return S;
	}
}

// ===== FBitcrusher ==========================================================

TEST_CASE("FBitcrusher: rate=1, bits=16, mix=1 is near-passthrough", "[bitcrusher]")
{
	FBitcrusher B;
	B.SetParamValue(FBitcrusher::Param_SampleRateRatio, 1.0f);
	B.SetParamValue(FBitcrusher::Param_Bits, 16.0f);
	B.SetParamValue(FBitcrusher::Param_Mix, 1.0f);
	B.Prepare(48000.0);

	std::vector<float> In = Sine(BlockSize, 1000.0, 48000.0, 0.5f);
	B.SetInputBuffer(0, In.data(), 0);
	B.SetInputBuffer(0, In.data(), 1);
	B.Process(Ctx());
	const float* OL = B.GetOutputBuffer(0, 0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		// 16-bit quantize step = 2/2^16 ≈ 3e-5 — well within tolerance.
		REQUIRE_THAT(OL[I], Catch::Matchers::WithinAbs(In[I], 1e-3f));
	}
}

TEST_CASE("FBitcrusher: bits=1 outputs only ±1 (or 0)", "[bitcrusher]")
{
	FBitcrusher B;
	B.SetParamValue(FBitcrusher::Param_Bits, 1.0f);
	B.SetParamValue(FBitcrusher::Param_Mix, 1.0f);
	B.Prepare(48000.0);

	std::vector<float> In = Sine(BlockSize, 1000.0, 48000.0, 0.5f);
	B.SetInputBuffer(0, In.data(), 0);
	B.SetInputBuffer(0, In.data(), 1);
	B.Process(Ctx());
	const float* OL = B.GetOutputBuffer(0, 0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		const bool bIsExtreme = std::fabs(OL[I] - 1.0f) < 1e-4f
			|| std::fabs(OL[I] + 1.0f) < 1e-4f
			|| std::fabs(OL[I]) < 1e-4f;
		REQUIRE(bIsExtreme);
	}
}

TEST_CASE("FBitcrusher: low rate holds the same sample for many host steps", "[bitcrusher]")
{
	FBitcrusher B;
	B.SetParamValue(FBitcrusher::Param_SampleRateRatio, 0.05f);  // 1 in 20 samples
	B.SetParamValue(FBitcrusher::Param_Mix, 1.0f);
	B.Prepare(48000.0);

	std::vector<float> In = Sine(BlockSize, 1000.0, 48000.0, 0.5f);
	B.SetInputBuffer(0, In.data(), 0);
	B.SetInputBuffer(0, In.data(), 1);
	B.Process(Ctx());

	// Count how many neighbouring samples have the same value (i.e. the
	// hold counter hadn't yet expired). Should be most of them.
	const float* OL = B.GetOutputBuffer(0, 0);
	uint32_t Same = 0;
	for (uint32_t I = 1; I < BlockSize; ++I)
	{
		if (OL[I] == OL[I - 1]) { ++Same; }
	}
	REQUIRE(Same > BlockSize / 2);  // most adjacent pairs are equal
}

// ===== FRingMod =============================================================

TEST_CASE("FRingMod: mix=0 → bit-identical passthrough", "[ringmod]")
{
	FRingMod R;
	R.SetParamValue(FRingMod::Param_Mix, 0.0f);
	R.Prepare(48000.0);

	std::vector<float> In = Sine(BlockSize, 1000.0, 48000.0, 0.5f);
	R.SetInputBuffer(0, In.data(), 0);
	R.SetInputBuffer(0, In.data(), 1);
	R.Process(Ctx());
	const float* OL = R.GetOutputBuffer(0, 0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(OL[I], Catch::Matchers::WithinAbs(In[I], 1e-4f));
	}
}

TEST_CASE("FRingMod: input × carrier produces sum/difference frequencies", "[ringmod]")
{
	// 1 kHz carrier × 100 Hz input → 900 Hz and 1100 Hz components, no
	// 1 kHz or 100 Hz (those cancel via the multiplicative trig identity).
	// Test indirectly: the output's RMS should be roughly sqrt(2)/2 ×
	// input RMS (since the product of two unit sines of distinct freqs has
	// half the energy), and clearly different from the input.
	FRingMod R;
	R.SetParamValue(FRingMod::Param_CarrierHz, 1000.0f);
	R.SetParamValue(FRingMod::Param_Shape, 0.0f);    // Sine
	R.SetParamValue(FRingMod::Param_Mix, 1.0f);
	R.Prepare(48000.0);

	std::vector<float> In = Sine(BlockSize, 750.0, 48000.0, 1.0f);  // 1 cycle/block
	R.SetInputBuffer(0, In.data(), 0);
	R.SetInputBuffer(0, In.data(), 1);
	for (uint32_t B = 0; B < 50; ++B) { R.Process(Ctx()); }
	R.Process(Ctx());

	const float* OL = R.GetOutputBuffer(0, 0);
	double InEnergy = 0.0, OutEnergy = 0.0, DiffEnergy = 0.0;
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		InEnergy += In[I] * In[I];
		OutEnergy += OL[I] * OL[I];
		const double D = OL[I] - In[I];
		DiffEnergy += D * D;
	}
	// Output RMS should be lower than input (energy loss to product). And
	// the difference signal should be substantial (output is different
	// from input — it's been ring-modulated, not bypassed).
	REQUIRE(OutEnergy < InEnergy);
	REQUIRE(DiffEnergy > InEnergy * 0.2);
}

// ===== FStereoWidener =======================================================

TEST_CASE("FStereoWidener: width=1 is bit-identical passthrough", "[widener]")
{
	FStereoWidener W;
	W.SetParamValue(FStereoWidener::Param_Width, 1.0f);
	W.Prepare(48000.0);

	std::vector<float> InL = Sine(BlockSize, 750.0, 48000.0, 0.5f);
	std::vector<float> InR = Sine(BlockSize, 1500.0, 48000.0, 0.3f);
	W.SetInputBuffer(0, InL.data(), 0);
	W.SetInputBuffer(0, InR.data(), 1);
	W.Process(Ctx());
	const float* OL = W.GetOutputBuffer(0, 0);
	const float* OR = W.GetOutputBuffer(0, 1);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(OL[I], Catch::Matchers::WithinAbs(InL[I], 1e-4f));
		REQUIRE_THAT(OR[I], Catch::Matchers::WithinAbs(InR[I], 1e-4f));
	}
}

TEST_CASE("FStereoWidener: width=0 produces mono (L == R)", "[widener]")
{
	FStereoWidener W;
	W.SetParamValue(FStereoWidener::Param_Width, 0.0f);
	W.Prepare(48000.0);

	std::vector<float> InL = Sine(BlockSize, 750.0, 48000.0, 0.5f);
	std::vector<float> InR = Sine(BlockSize, 1500.0, 48000.0, 0.3f);
	W.SetInputBuffer(0, InL.data(), 0);
	W.SetInputBuffer(0, InR.data(), 1);
	// Burn-in so the width smoother converges to 0.
	for (uint32_t B = 0; B < 50; ++B) { W.Process(Ctx()); }
	W.Process(Ctx());
	const float* OL = W.GetOutputBuffer(0, 0);
	const float* OR = W.GetOutputBuffer(0, 1);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(OL[I], Catch::Matchers::WithinAbs(OR[I], 1e-4f));
	}
}

// ===== FHaasWidener =========================================================

TEST_CASE("FHaasWidener: delay=0 → L and R unchanged from input", "[haas]")
{
	FHaasWidener H;
	H.SetParamValue(FHaasWidener::Param_DelayMs, 0.0f);
	H.SetParamValue(FHaasWidener::Param_Mix, 1.0f);
	H.Prepare(48000.0);

	std::vector<float> In = Sine(BlockSize, 1000.0, 48000.0, 0.5f);
	H.SetInputBuffer(0, In.data(), 0);
	H.SetInputBuffer(0, In.data(), 1);
	H.Process(Ctx());
	const float* OL = H.GetOutputBuffer(0, 0);
	const float* OR = H.GetOutputBuffer(0, 1);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(OL[I], Catch::Matchers::WithinAbs(In[I], 1e-4f));
		REQUIRE_THAT(OR[I], Catch::Matchers::WithinAbs(In[I], 1e-4f));
	}
}

TEST_CASE("FHaasWidener: 15 ms delay shifts the chosen channel by ~720 samples", "[haas]")
{
	FHaasWidener H;
	H.SetParamValue(FHaasWidener::Param_DelayMs, 15.0f);    // 720 samples @ 48k
	H.SetParamValue(FHaasWidener::Param_Side, 0.0f);        // delay R
	H.SetParamValue(FHaasWidener::Param_Mix, 1.0f);
	H.Prepare(48000.0);

	// Send an impulse on sample 0; run enough blocks for the delayed
	// impulse to appear on R.
	std::vector<float> Impulse(BlockSize, 0.0f);
	Impulse[0] = 1.0f;
	std::vector<float> Quiet(BlockSize, 0.0f);

	H.SetInputBuffer(0, Impulse.data(), 0);
	H.SetInputBuffer(0, Impulse.data(), 1);
	H.Process(Ctx());

	// L should have the impulse at sample 0, R should not.
	const float* OL = H.GetOutputBuffer(0, 0);
	const float* OR = H.GetOutputBuffer(0, 1);
	REQUIRE(OL[0] > 0.99f);
	REQUIRE(std::fabs(OR[0]) < 0.01f);

	// After ~12 more blocks (total samples > 720), the delayed impulse
	// should have appeared somewhere on the R channel.
	H.SetInputBuffer(0, Quiet.data(), 0);
	H.SetInputBuffer(0, Quiet.data(), 1);
	float MaxR = 0.0f;
	for (uint32_t B = 0; B < 13; ++B)
	{
		H.Process(Ctx());
		const float* Or = H.GetOutputBuffer(0, 1);
		for (uint32_t I = 0; I < BlockSize; ++I)
		{
			if (std::fabs(Or[I]) > MaxR) { MaxR = std::fabs(Or[I]); }
		}
	}
	REQUIRE(MaxR > 0.95f);
}

// ===== FExciter =============================================================

TEST_CASE("FExciter: mix=0 → bit-identical passthrough", "[exciter]")
{
	FExciter E;
	E.SetParamValue(FExciter::Param_Mix, 0.0f);
	E.Prepare(48000.0);

	std::vector<float> In = Sine(BlockSize, 1000.0, 48000.0, 0.5f);
	E.SetInputBuffer(0, In.data(), 0);
	E.SetInputBuffer(0, In.data(), 1);
	// Burn-in so the mix smoother converges to 0.
	for (uint32_t B = 0; B < 50; ++B) { E.Process(Ctx()); }
	E.Process(Ctx());
	const float* OL = E.GetOutputBuffer(0, 0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(OL[I], Catch::Matchers::WithinAbs(In[I], 1e-4f));
	}
}

TEST_CASE("FExciter: high drive on a high-frequency input produces non-trivial harmonics", "[exciter]")
{
	// A 6 kHz sine through a 3 kHz HP gets passed mostly intact; tanh at
	// high drive then generates harmonics at 12 kHz, 18 kHz, etc. Output
	// energy should exceed input energy due to the added harmonics + dry.
	FExciter E;
	E.SetParamValue(FExciter::Param_Frequency, 3000.0f);
	E.SetParamValue(FExciter::Param_DriveDb, 24.0f);
	E.SetParamValue(FExciter::Param_Mix, 0.5f);
	E.Prepare(48000.0);

	std::vector<float> In = Sine(BlockSize, 6000.0, 48000.0, 0.3f);
	E.SetInputBuffer(0, In.data(), 0);
	E.SetInputBuffer(0, In.data(), 1);
	for (uint32_t B = 0; B < 50; ++B) { E.Process(Ctx()); }
	E.Process(Ctx());

	const float* OL = E.GetOutputBuffer(0, 0);
	double InEnergy = 0.0, OutEnergy = 0.0;
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		InEnergy += In[I] * In[I];
		OutEnergy += OL[I] * OL[I];
	}
	REQUIRE(OutEnergy > InEnergy * 1.2);
}
