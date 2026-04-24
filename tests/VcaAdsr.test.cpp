#include "dsp/Adsr.h"
#include "dsp/Vca.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>

using namespace NodeSynth;

TEST_CASE("FVca multiplies audio by control buffer", "[vca]")
{
	FVca Vca;
	alignas(16) float AudioBuf[BlockSize];
	alignas(16) float CtrlBuf[BlockSize];
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		AudioBuf[I] = 0.5f;
		CtrlBuf[I] = 0.25f;
	}
	Vca.SetInputBuffer(0, AudioBuf);
	Vca.SetInputBuffer(1, CtrlBuf);

	FProcessContext Ctx;
	Ctx.BlockSize = BlockSize;
	Ctx.SampleRate = 48000.0;
	Vca.Process(Ctx);

	const float* Out = Vca.GetOutputBuffer(0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(Out[I], Catch::Matchers::WithinAbs(0.125f, 1e-6f));
	}
}

TEST_CASE("FVca with disconnected control passes audio through", "[vca]")
{
	FVca Vca;
	alignas(16) float AudioBuf[BlockSize];
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		AudioBuf[I] = 0.7f;
	}
	Vca.SetInputBuffer(0, AudioBuf);
	Vca.SetInputBuffer(1, nullptr);

	FProcessContext Ctx;
	Ctx.BlockSize = BlockSize;
	Ctx.SampleRate = 48000.0;
	Vca.Process(Ctx);

	const float* Out = Vca.GetOutputBuffer(0);
	REQUIRE_THAT(Out[0], Catch::Matchers::WithinAbs(0.7f, 1e-6f));
}

TEST_CASE("FAdsr idles silent until gate rises", "[adsr]")
{
	FAdsr Adsr;
	Adsr.Prepare(48000.0);

	FProcessContext Ctx;
	Ctx.BlockSize = BlockSize;
	Ctx.SampleRate = 48000.0;
	Adsr.SetInputBuffer(0, nullptr);
	Adsr.Process(Ctx);

	const float* Out = Adsr.GetOutputBuffer(0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE(Out[I] == 0.0f);
	}
}

TEST_CASE("FAdsr attacks to 1 and decays to sustain on held gate", "[adsr]")
{
	FAdsr Adsr;
	Adsr.SetParamValue(FAdsr::Param_AttackMs, 1.0f);
	Adsr.SetParamValue(FAdsr::Param_DecayMs, 2.0f);
	Adsr.SetParamValue(FAdsr::Param_Sustain, 0.5f);
	Adsr.SetParamValue(FAdsr::Param_ReleaseMs, 10.0f);
	Adsr.Prepare(48000.0);

	// Drive with a held gate long enough to reach sustain.
	alignas(16) float GateBuf[BlockSize];
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		GateBuf[I] = 1.0f;
	}
	Adsr.SetInputBuffer(0, GateBuf);

	FProcessContext Ctx;
	Ctx.BlockSize = BlockSize;
	Ctx.SampleRate = 48000.0;

	// 100 blocks @ 48k / 64 = ~133ms, plenty for 1ms attack + 2ms decay.
	for (int B = 0; B < 100; ++B)
	{
		Adsr.Process(Ctx);
	}
	const float* Out = Adsr.GetOutputBuffer(0);
	REQUIRE_THAT(Out[BlockSize - 1], Catch::Matchers::WithinAbs(0.5f, 1e-3f));
	REQUIRE(Adsr.GetStage() == FAdsr::EStage::Sustain);
}

TEST_CASE("FAdsr falls to zero after gate release", "[adsr]")
{
	FAdsr Adsr;
	Adsr.SetParamValue(FAdsr::Param_AttackMs, 0.5f);
	Adsr.SetParamValue(FAdsr::Param_DecayMs, 1.0f);
	Adsr.SetParamValue(FAdsr::Param_Sustain, 0.8f);
	Adsr.SetParamValue(FAdsr::Param_ReleaseMs, 2.0f);
	Adsr.Prepare(48000.0);

	alignas(16) float GateBuf[BlockSize];
	FProcessContext Ctx;
	Ctx.BlockSize = BlockSize;
	Ctx.SampleRate = 48000.0;
	Adsr.SetInputBuffer(0, GateBuf);

	// Gate high for a while — reach sustain.
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		GateBuf[I] = 1.0f;
	}
	for (int B = 0; B < 50; ++B)
	{
		Adsr.Process(Ctx);
	}

	// Gate goes low — release.
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		GateBuf[I] = 0.0f;
	}
	for (int B = 0; B < 100; ++B)
	{
		Adsr.Process(Ctx);
	}

	const float* Out = Adsr.GetOutputBuffer(0);
	REQUIRE_THAT(Out[BlockSize - 1], Catch::Matchers::WithinAbs(0.0f, 1e-3f));
	REQUIRE(Adsr.GetStage() == FAdsr::EStage::Idle);
}
