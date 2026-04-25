#include "dsp/Add.h"
#include "dsp/Constant.h"
#include "dsp/Multiply.h"
#include "dsp/SampleHold.h"
#include "dsp/Scale.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <vector>

using NodeSynth::BlockSize;
using NodeSynth::FAdd;
using NodeSynth::FConstant;
using NodeSynth::FMultiply;
using NodeSynth::FProcessContext;
using NodeSynth::FSampleHold;
using NodeSynth::FScale;

namespace
{
	std::vector<float> Constant(float Value, uint32_t N = BlockSize)
	{
		return std::vector<float>(N, Value);
	}

	FProcessContext Ctx(uint32_t Block = BlockSize, double Sr = 48000.0)
	{
		FProcessContext C;
		C.BlockSize = Block;
		C.SampleRate = Sr;
		return C;
	}
}

// -- FAdd ----------------------------------------------------------------------

TEST_CASE("FAdd: A + B per sample", "[math][add]")
{
	FAdd Node;
	Node.Prepare(48000.0);
	const auto A = Constant(0.25f);
	const auto B = Constant(0.5f);
	Node.SetInputBuffer(0, A.data());
	Node.SetInputBuffer(1, B.data());
	Node.Process(Ctx());

	const float* Out = Node.GetOutputBuffer(0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(Out[I], Catch::Matchers::WithinAbs(0.75f, 1e-6f));
	}
}

TEST_CASE("FAdd: disconnected inputs read as 0", "[math][add]")
{
	FAdd Node;
	Node.Prepare(48000.0);
	const auto A = Constant(0.3f);
	Node.SetInputBuffer(0, A.data());
	// B left disconnected.
	Node.Process(Ctx());

	const float* Out = Node.GetOutputBuffer(0);
	REQUIRE_THAT(Out[0], Catch::Matchers::WithinAbs(0.3f, 1e-6f));
}

// -- FMultiply -----------------------------------------------------------------

TEST_CASE("FMultiply: A * B per sample", "[math][multiply]")
{
	FMultiply Node;
	Node.Prepare(48000.0);
	const auto A = Constant(2.0f);
	const auto B = Constant(0.5f);
	Node.SetInputBuffer(0, A.data());
	Node.SetInputBuffer(1, B.data());
	Node.Process(Ctx());

	const float* Out = Node.GetOutputBuffer(0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(Out[I], Catch::Matchers::WithinAbs(1.0f, 1e-6f));
	}
}

TEST_CASE("FMultiply: disconnected inputs read as 1 (identity)", "[math][multiply]")
{
	FMultiply Node;
	Node.Prepare(48000.0);
	const auto A = Constant(0.7f);
	Node.SetInputBuffer(0, A.data());
	// B left disconnected — should pass A through unchanged.
	Node.Process(Ctx());

	const float* Out = Node.GetOutputBuffer(0);
	REQUIRE_THAT(Out[0], Catch::Matchers::WithinAbs(0.7f, 1e-6f));
}

// -- FScale --------------------------------------------------------------------

TEST_CASE("FScale: default remaps [-1, 1] to [0, 1]", "[math][scale]")
{
	FScale Node;
	Node.Prepare(48000.0);
	const auto In = Constant(-1.0f);
	Node.SetInputBuffer(0, In.data());
	Node.Process(Ctx());
	REQUIRE_THAT(Node.GetOutputBuffer(0)[0], Catch::Matchers::WithinAbs(0.0f, 1e-6f));

	const auto In2 = Constant(1.0f);
	Node.SetInputBuffer(0, In2.data());
	Node.Process(Ctx());
	REQUIRE_THAT(Node.GetOutputBuffer(0)[0], Catch::Matchers::WithinAbs(1.0f, 1e-6f));

	const auto In3 = Constant(0.0f);
	Node.SetInputBuffer(0, In3.data());
	Node.Process(Ctx());
	REQUIRE_THAT(Node.GetOutputBuffer(0)[0], Catch::Matchers::WithinAbs(0.5f, 1e-6f));
}

TEST_CASE("FScale: extrapolates outside the input range", "[math][scale]")
{
	FScale Node;
	Node.Prepare(48000.0);
	// Identity remap so we can read extrapolated values directly.
	Node.SetParamValue(FScale::Param_InMin, 0.0f);
	Node.SetParamValue(FScale::Param_InMax, 1.0f);
	Node.SetParamValue(FScale::Param_OutMin, 0.0f);
	Node.SetParamValue(FScale::Param_OutMax, 1.0f);

	const auto In = Constant(2.5f);
	Node.SetInputBuffer(0, In.data());
	Node.Process(Ctx());
	REQUIRE_THAT(Node.GetOutputBuffer(0)[0], Catch::Matchers::WithinAbs(2.5f, 1e-6f));
}

TEST_CASE("FScale: degenerate input range emits OutMin (no NaN)", "[math][scale]")
{
	FScale Node;
	Node.Prepare(48000.0);
	Node.SetParamValue(FScale::Param_InMin, 0.5f);
	Node.SetParamValue(FScale::Param_InMax, 0.5f); // span = 0
	Node.SetParamValue(FScale::Param_OutMin, -1.0f);
	Node.SetParamValue(FScale::Param_OutMax, 1.0f);

	const auto In = Constant(0.5f);
	Node.SetInputBuffer(0, In.data());
	Node.Process(Ctx());
	REQUIRE_THAT(Node.GetOutputBuffer(0)[0], Catch::Matchers::WithinAbs(-1.0f, 1e-6f));
}

// -- FConstant -----------------------------------------------------------------

TEST_CASE("FConstant: outputs the Value param continuously", "[math][constant]")
{
	FConstant Node;
	Node.Prepare(48000.0);
	Node.SetParamValue(FConstant::Param_Value, 0.42f);
	Node.Process(Ctx());

	const float* Out = Node.GetOutputBuffer(0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(Out[I], Catch::Matchers::WithinAbs(0.42f, 1e-6f));
	}
}

// -- FSampleHold ---------------------------------------------------------------

TEST_CASE("FSampleHold: latches In on the rising edge of Trigger", "[math][samplehold]")
{
	FSampleHold Node;
	Node.Prepare(48000.0);

	std::vector<float> In(BlockSize, 0.0f);
	std::vector<float> Trig(BlockSize, 0.0f);
	// Mark sample 5 as the first rising edge with In = 0.7.
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		In[I] = (I < 10) ? 0.7f : 0.3f;
		Trig[I] = (I >= 5 && I < 7) ? 1.0f : 0.0f; // rising edge at I=5, falling at I=7
	}
	Node.SetInputBuffer(FSampleHold::Input_In, In.data());
	Node.SetInputBuffer(FSampleHold::Input_Trigger, Trig.data());
	Node.Process(Ctx());

	const float* Out = Node.GetOutputBuffer(0);
	REQUIRE_THAT(Out[0], Catch::Matchers::WithinAbs(0.0f, 1e-6f)); // not yet triggered
	REQUIRE_THAT(Out[5], Catch::Matchers::WithinAbs(0.7f, 1e-6f)); // latched on rising edge
	REQUIRE_THAT(Out[15], Catch::Matchers::WithinAbs(0.7f, 1e-6f)); // still held — no new edge
	REQUIRE_THAT(Out[BlockSize - 1], Catch::Matchers::WithinAbs(0.7f, 1e-6f));
}

TEST_CASE("FSampleHold: each rising edge re-latches", "[math][samplehold]")
{
	FSampleHold Node;
	Node.Prepare(48000.0);

	std::vector<float> In(BlockSize, 0.0f);
	std::vector<float> Trig(BlockSize, 0.0f);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		In[I] = static_cast<float>(I) * 0.01f;
	}
	// Rising edges at I=10, I=30.
	for (uint32_t I = 10; I < 12; ++I) { Trig[I] = 1.0f; }
	for (uint32_t I = 30; I < 32; ++I) { Trig[I] = 1.0f; }
	Node.SetInputBuffer(FSampleHold::Input_In, In.data());
	Node.SetInputBuffer(FSampleHold::Input_Trigger, Trig.data());
	Node.Process(Ctx());

	const float* Out = Node.GetOutputBuffer(0);
	REQUIRE_THAT(Out[10], Catch::Matchers::WithinAbs(0.10f, 1e-6f));
	REQUIRE_THAT(Out[20], Catch::Matchers::WithinAbs(0.10f, 1e-6f));
	REQUIRE_THAT(Out[30], Catch::Matchers::WithinAbs(0.30f, 1e-6f));
	REQUIRE_THAT(Out[BlockSize - 1], Catch::Matchers::WithinAbs(0.30f, 1e-6f));
}

TEST_CASE("FSampleHold: held state persists across blocks", "[math][samplehold]")
{
	FSampleHold Node;
	Node.Prepare(48000.0);

	std::vector<float> In(BlockSize, 0.42f);
	std::vector<float> Trig(BlockSize, 0.0f);
	Trig[3] = 1.0f; // single rising edge in the first block
	Node.SetInputBuffer(FSampleHold::Input_In, In.data());
	Node.SetInputBuffer(FSampleHold::Input_Trigger, Trig.data());
	Node.Process(Ctx());
	REQUIRE_THAT(Node.GetOutputBuffer(0)[BlockSize - 1], Catch::Matchers::WithinAbs(0.42f, 1e-6f));

	// Second block: no triggers, In changes — output must keep the old held value.
	std::vector<float> In2(BlockSize, 0.99f);
	std::vector<float> Trig2(BlockSize, 0.0f);
	Node.SetInputBuffer(FSampleHold::Input_In, In2.data());
	Node.SetInputBuffer(FSampleHold::Input_Trigger, Trig2.data());
	Node.Process(Ctx());
	REQUIRE_THAT(Node.GetOutputBuffer(0)[0], Catch::Matchers::WithinAbs(0.42f, 1e-6f));
	REQUIRE_THAT(Node.GetOutputBuffer(0)[BlockSize - 1], Catch::Matchers::WithinAbs(0.42f, 1e-6f));
}
