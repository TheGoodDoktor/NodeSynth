#include "dsp/MidiCC.h"
#include "dsp/Node.h"
#include "ui/NodeRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>

using namespace NodeSynth;

namespace
{
	// Run the smoother long enough for it to settle within a tolerance.
	void StepUntilSettle(FMidiCC& Cc, float Target, float Tol = 1e-3f,
		uint32_t MaxBlocks = 200)
	{
		FProcessContext Ctx;
		for (uint32_t B = 0; B < MaxBlocks; ++B)
		{
			Cc.Process(Ctx);
			float* Out = Cc.GetOutputBuffer(0);
			if (std::fabs(Out[BlockSize - 1] - Target) <= Tol)
			{
				return;
			}
		}
	}
}

TEST_CASE("FMidiCC default state outputs Min", "[midicc]")
{
	FMidiCC Cc;
	Cc.SetParamValue(FMidiCC::Param_Min, 0.0f);
	Cc.SetParamValue(FMidiCC::Param_Max, 1.0f);
	Cc.Prepare(48000.0);
	StepUntilSettle(Cc, 0.0f);
	float* Out = Cc.GetOutputBuffer(0);
	REQUIRE_THAT(Out[BlockSize - 1], Catch::Matchers::WithinAbs(0.0f, 1e-3f));
}

TEST_CASE("FMidiCC raw 127 outputs Max", "[midicc]")
{
	FMidiCC Cc;
	Cc.SetParamValue(FMidiCC::Param_Cc, 7.0f);
	Cc.SetParamValue(FMidiCC::Param_Min, 0.0f);
	Cc.SetParamValue(FMidiCC::Param_Max, 1.0f);
	Cc.Prepare(48000.0);
	Cc.OnCcEvent(1 /*channel*/, 7 /*cc*/, 127);
	StepUntilSettle(Cc, 1.0f);
	float* Out = Cc.GetOutputBuffer(0);
	REQUIRE_THAT(Out[BlockSize - 1], Catch::Matchers::WithinAbs(1.0f, 5e-3f));
}

TEST_CASE("FMidiCC midpoint scales correctly with custom Min/Max", "[midicc]")
{
	FMidiCC Cc;
	Cc.SetParamValue(FMidiCC::Param_Cc, 74.0f);
	Cc.SetParamValue(FMidiCC::Param_Min, 200.0f);
	Cc.SetParamValue(FMidiCC::Param_Max, 8000.0f);
	Cc.Prepare(48000.0);
	Cc.OnCcEvent(1, 74, 64);
	// 64/127 * (8000 - 200) + 200 = 4129.13...
	StepUntilSettle(Cc, 4129.13f, 10.0f);
	float* Out = Cc.GetOutputBuffer(0);
	REQUIRE_THAT(Out[BlockSize - 1], Catch::Matchers::WithinAbs(4129.13f, 50.0f));
}

TEST_CASE("FMidiCC ignores wrong CC number", "[midicc]")
{
	FMidiCC Cc;
	Cc.SetParamValue(FMidiCC::Param_Cc, 7.0f);
	Cc.SetParamValue(FMidiCC::Param_Min, 0.0f);
	Cc.SetParamValue(FMidiCC::Param_Max, 1.0f);
	Cc.Prepare(48000.0);
	Cc.OnCcEvent(1, 11 /*different cc*/, 127);
	StepUntilSettle(Cc, 0.0f);
	float* Out = Cc.GetOutputBuffer(0);
	REQUIRE_THAT(Out[BlockSize - 1], Catch::Matchers::WithinAbs(0.0f, 1e-3f));
}

TEST_CASE("FMidiCC ignores wrong channel when filter set", "[midicc]")
{
	FMidiCC Cc;
	Cc.SetParamValue(FMidiCC::Param_Cc, 7.0f);
	Cc.SetParamValue(FMidiCC::Param_Channel, 2.0f);  // channel 2 only
	Cc.SetParamValue(FMidiCC::Param_Min, 0.0f);
	Cc.SetParamValue(FMidiCC::Param_Max, 1.0f);
	Cc.Prepare(48000.0);
	Cc.OnCcEvent(5 /*channel 5*/, 7, 127);
	StepUntilSettle(Cc, 0.0f);
	float* Out = Cc.GetOutputBuffer(0);
	REQUIRE_THAT(Out[BlockSize - 1], Catch::Matchers::WithinAbs(0.0f, 1e-3f));
}

TEST_CASE("FMidiCC Omni accepts any channel", "[midicc]")
{
	FMidiCC Cc;
	Cc.SetParamValue(FMidiCC::Param_Cc, 7.0f);
	Cc.SetParamValue(FMidiCC::Param_Channel, 0.0f);  // Omni
	Cc.SetParamValue(FMidiCC::Param_Min, 0.0f);
	Cc.SetParamValue(FMidiCC::Param_Max, 1.0f);
	Cc.Prepare(48000.0);
	Cc.OnCcEvent(5, 7, 127);  // any channel works
	StepUntilSettle(Cc, 1.0f);
	float* Out = Cc.GetOutputBuffer(0);
	REQUIRE_THAT(Out[BlockSize - 1], Catch::Matchers::WithinAbs(1.0f, 5e-3f));
}

TEST_CASE("FMidiCC smooths step into a ramp", "[midicc]")
{
	FMidiCC Cc;
	Cc.SetParamValue(FMidiCC::Param_Cc, 7.0f);
	Cc.SetParamValue(FMidiCC::Param_Min, 0.0f);
	Cc.SetParamValue(FMidiCC::Param_Max, 1.0f);
	Cc.SetParamValue(FMidiCC::Param_SmoothMs, 50.0f);  // slow ramp
	Cc.Prepare(48000.0);
	Cc.OnCcEvent(1, 7, 127);

	// After one block (~1.3 ms) the smoother should still be ramping —
	// not at 0, not at 1.
	FProcessContext Ctx;
	Cc.Process(Ctx);
	float* Out = Cc.GetOutputBuffer(0);
	const float Mid = Out[BlockSize - 1];
	REQUIRE(Mid > 0.0f);
	REQUIRE(Mid < 1.0f);
}

TEST_CASE("FMidiCC registers in the palette under Modulation", "[midicc]")
{
	const auto& Reg = GetNodeRegistry();
	auto It = std::find_if(Reg.begin(), Reg.end(),
		[](const FNodeRegistration& E)
		{
			return std::string(E.TypeName) == "MidiCC";
		});
	REQUIRE(It != Reg.end());
	REQUIRE(std::string(It->Category) == "Modulation");
	auto N = It->Make();
	REQUIRE(N);
	REQUIRE(std::string(N->GetTypeName()) == "MidiCC");
}
