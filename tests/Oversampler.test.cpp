#include "dsp/Oversampler.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using NodeSynth::BlockSize;
using NodeSynth::FOversampler2x;

namespace
{
	// Identity inner process: leave the upsampled buffer unchanged so the
	// round-trip exposes the FIR's response without DSP modification.
	auto Identity = [](float* /*Buf*/, uint32_t /*N*/) {};
}

TEST_CASE("FOversampler2x: zero input produces zero output", "[oversampler]")
{
	FOversampler2x Os;
	Os.Prepare(48000.0);

	std::vector<float> In(BlockSize, 0.0f);
	std::vector<float> Out(BlockSize, 0.0f);
	Os.Process(In.data(), Out.data(), BlockSize, Identity);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE(Out[I] == 0.0f);
	}
}

TEST_CASE("FOversampler2x: DC input round-trips to DC after settling", "[oversampler]")
{
	FOversampler2x Os;
	Os.Prepare(48000.0);

	const float Dc = 0.5f;
	std::vector<float> In(BlockSize, Dc);
	std::vector<float> Out(BlockSize, 0.0f);

	// Run several blocks so the FIR's transient settles; then verify the
	// last block is approximately DC at the input level. The total round-trip
	// has unity DC gain by construction (coefficients sum to 1).
	for (uint32_t B = 0; B < 8; ++B)
	{
		Os.Process(In.data(), Out.data(), BlockSize, Identity);
	}
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(Out[I], Catch::Matchers::WithinAbs(Dc, 0.01f));
	}
}

TEST_CASE("FOversampler2x: low-frequency sine round-trips with low error", "[oversampler]")
{
	FOversampler2x Os;
	Os.Prepare(48000.0);

	// 1 kHz sine at 48 kHz: well within the half-band passband, so should
	// round-trip with low error after the FIR settles.
	const double Freq = 1000.0;
	const double Sr = 48000.0;
	const double TwoPi = 2.0 * 3.141592653589793;
	const double Phase = TwoPi * Freq / Sr;

	std::vector<float> Out(BlockSize, 0.0f);
	std::vector<float> InBuf(BlockSize, 0.0f);
	double T = 0.0;

	// Burn-in blocks to settle the FIR group delay.
	for (uint32_t B = 0; B < 4; ++B)
	{
		for (uint32_t I = 0; I < BlockSize; ++I)
		{
			InBuf[I] = static_cast<float>(std::sin(T));
			T += Phase;
		}
		Os.Process(InBuf.data(), Out.data(), BlockSize, Identity);
	}

	// One more block for measurement. Compare RMS energy in vs out — should
	// be very close (passband is flat to within ~0.5 dB).
	double InEnergy = 0.0, OutEnergy = 0.0;
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		InBuf[I] = static_cast<float>(std::sin(T));
		T += Phase;
		InEnergy += InBuf[I] * InBuf[I];
	}
	Os.Process(InBuf.data(), Out.data(), BlockSize, Identity);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		OutEnergy += Out[I] * Out[I];
	}
	const double Ratio = OutEnergy / InEnergy;
	// Passband flat to within ~10% (≈ 0.4 dB).
	REQUIRE(Ratio > 0.85);
	REQUIRE(Ratio < 1.15);
}

TEST_CASE("FOversampler2x: inner lambda receives a 2*N buffer", "[oversampler]")
{
	FOversampler2x Os;
	Os.Prepare(48000.0);

	std::vector<float> In(BlockSize, 1.0f);
	std::vector<float> Out(BlockSize, 0.0f);

	uint32_t SeenSize = 0;
	bool bSeenAtAll = false;
	Os.Process(In.data(), Out.data(), BlockSize,
		[&](float* /*Buf*/, uint32_t N)
		{
			SeenSize = N;
			bSeenAtAll = true;
		});

	REQUIRE(bSeenAtAll);
	REQUIRE(SeenSize == BlockSize * 2);
}

TEST_CASE("FOversampler2x: inner lambda can modify the buffer", "[oversampler]")
{
	FOversampler2x Os;
	Os.Prepare(48000.0);

	std::vector<float> In(BlockSize, 1.0f);
	std::vector<float> Out(BlockSize, 0.0f);

	// Run several settle blocks first.
	for (uint32_t B = 0; B < 8; ++B)
	{
		Os.Process(In.data(), Out.data(), BlockSize, Identity);
	}
	const float DcIdent = Out[BlockSize / 2];

	// Now run with an inner that scales by 2.
	for (uint32_t B = 0; B < 8; ++B)
	{
		Os.Process(In.data(), Out.data(), BlockSize,
			[](float* Buf, uint32_t N)
			{
				for (uint32_t I = 0; I < N; ++I) { Buf[I] *= 2.0f; }
			});
	}
	const float DcScaled = Out[BlockSize / 2];
	REQUIRE_THAT(DcScaled, Catch::Matchers::WithinAbs(DcIdent * 2.0f, 0.01f));
}
