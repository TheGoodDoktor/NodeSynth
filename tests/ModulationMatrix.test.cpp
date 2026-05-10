#include "dsp/Constant.h"
#include "dsp/ModulationMatrix.h"
#include "dsp/Node.h"
#include "ui/NodeRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>

using namespace NodeSynth;

namespace
{
	// Run blocks until output port settles within tolerance, then return
	// the steady-state value at the last sample.
	float SettleAndRead(FModulationMatrix& M, uint32_t Port,
		float Target, uint32_t MaxBlocks = 200, float Tol = 1e-3f)
	{
		FProcessContext Ctx;
		float Last = 0.0f;
		for (uint32_t B = 0; B < MaxBlocks; ++B)
		{
			M.Process(Ctx);
			Last = M.GetOutputBuffer(Port)[BlockSize - 1];
			if (std::fabs(Last - Target) <= Tol) { return Last; }
		}
		return Last;
	}

	// Synthesize a constant input buffer. A simple way to feed a CV
	// source into the matrix without a real Constant node + plumbing.
	struct FConstBuf
	{
		std::array<float, BlockSize> Data{};
		explicit FConstBuf(float V) { Data.fill(V); }
		const float* Ptr() const { return Data.data(); }
	};
}

TEST_CASE("FModulationMatrix routes single source with depth 1", "[modmatrix]")
{
	FModulationMatrix M;
	M.SetParamValue(FModulationMatrix::DepthIndex(0, 0), 1.0f);
	M.Prepare(48000.0);

	FConstBuf One(1.0f);
	M.SetInputBuffer(0, One.Ptr());

	const float V = SettleAndRead(M, 0, 1.0f);
	REQUIRE_THAT(V, Catch::Matchers::WithinAbs(1.0f, 5e-3f));
}

TEST_CASE("FModulationMatrix sums depth-weighted sources", "[modmatrix]")
{
	FModulationMatrix M;
	// Out0 = 0.5 * Src0 + 0.25 * Src1
	M.SetParamValue(FModulationMatrix::DepthIndex(0, 0), 0.5f);
	M.SetParamValue(FModulationMatrix::DepthIndex(0, 1), 0.25f);
	M.Prepare(48000.0);

	FConstBuf One(1.0f);
	FConstBuf Two(0.8f);  // can't exceed 1.0 in clamped depth math
	M.SetInputBuffer(0, One.Ptr());
	M.SetInputBuffer(1, Two.Ptr());

	const float Expected = 0.5f * 1.0f + 0.25f * 0.8f;  // 0.7
	REQUIRE_THAT(SettleAndRead(M, 0, Expected), Catch::Matchers::WithinAbs(Expected, 5e-3f));
}

TEST_CASE("FModulationMatrix negative depth inverts source", "[modmatrix]")
{
	FModulationMatrix M;
	M.SetParamValue(FModulationMatrix::DepthIndex(0, 0), -1.0f);
	M.Prepare(48000.0);

	FConstBuf One(1.0f);
	M.SetInputBuffer(0, One.Ptr());

	REQUIRE_THAT(SettleAndRead(M, 0, -1.0f), Catch::Matchers::WithinAbs(-1.0f, 5e-3f));
}

TEST_CASE("FModulationMatrix offset adds DC", "[modmatrix]")
{
	FModulationMatrix M;
	M.SetParamValue(FModulationMatrix::OffsetIndex(0), 0.7f);
	M.Prepare(48000.0);

	// No inputs wired; output should settle to offset only.
	REQUIRE_THAT(SettleAndRead(M, 0, 0.7f), Catch::Matchers::WithinAbs(0.7f, 5e-3f));
}

TEST_CASE("FModulationMatrix disconnected source contributes zero", "[modmatrix]")
{
	FModulationMatrix M;
	// Src0 wired with depth 0.5; Src5 unwired with depth 0.9 (must be ignored).
	M.SetParamValue(FModulationMatrix::DepthIndex(0, 0), 0.5f);
	M.SetParamValue(FModulationMatrix::DepthIndex(0, 5), 0.9f);
	M.Prepare(48000.0);

	FConstBuf One(1.0f);
	M.SetInputBuffer(0, One.Ptr());
	// Note: input 5 not set — GetInputBuffer returns nullptr.

	REQUIRE_THAT(SettleAndRead(M, 0, 0.5f), Catch::Matchers::WithinAbs(0.5f, 5e-3f));
}

TEST_CASE("FModulationMatrix routes independent destinations", "[modmatrix]")
{
	FModulationMatrix M;
	// Out0 = 0.5 * Src0; Out1 = 0.25 * Src0
	M.SetParamValue(FModulationMatrix::DepthIndex(0, 0), 0.5f);
	M.SetParamValue(FModulationMatrix::DepthIndex(1, 0), 0.25f);
	M.Prepare(48000.0);

	FConstBuf One(1.0f);
	M.SetInputBuffer(0, One.Ptr());

	// Drive both outputs; settle them in lockstep.
	FProcessContext Ctx;
	for (uint32_t B = 0; B < 200; ++B) { M.Process(Ctx); }
	REQUIRE_THAT(M.GetOutputBuffer(0)[BlockSize - 1], Catch::Matchers::WithinAbs(0.5f, 5e-3f));
	REQUIRE_THAT(M.GetOutputBuffer(1)[BlockSize - 1], Catch::Matchers::WithinAbs(0.25f, 5e-3f));
}

TEST_CASE("FModulationMatrix clamps depth and offset to [-1, 1]", "[modmatrix]")
{
	FModulationMatrix M;
	M.SetParamValue(FModulationMatrix::DepthIndex(0, 0), 5.0f);  // → 1.0
	M.SetParamValue(FModulationMatrix::OffsetIndex(1), -3.0f);   // → -1.0
	REQUIRE(M.GetParamValue(FModulationMatrix::DepthIndex(0, 0)) == 1.0f);
	REQUIRE(M.GetParamValue(FModulationMatrix::OffsetIndex(1)) == -1.0f);
}

TEST_CASE("FModulationMatrix Clone preserves params", "[modmatrix]")
{
	auto M = std::make_shared<FModulationMatrix>();
	M->SetParamValue(FModulationMatrix::DepthIndex(2, 3), 0.4f);
	M->SetParamValue(FModulationMatrix::OffsetIndex(2), 0.6f);

	auto Cloned = M->Clone();
	REQUIRE(Cloned);
	auto* C = dynamic_cast<FModulationMatrix*>(Cloned.get());
	REQUIRE(C);
	REQUIRE(C->GetParamValue(FModulationMatrix::DepthIndex(2, 3)) == 0.4f);
	REQUIRE(C->GetParamValue(FModulationMatrix::OffsetIndex(2)) == 0.6f);
}

TEST_CASE("FModulationMatrix all params hidden", "[modmatrix]")
{
	FModulationMatrix M;
	const auto Infos = M.GetParamInfos();
	REQUIRE(Infos.size() == FModulationMatrix::Param_COUNT);
	for (const FParamInfo& Info : Infos)
	{
		REQUIRE(Info.bHidden);
	}
}

TEST_CASE("FModulationMatrix registers under Modulation", "[modmatrix]")
{
	const auto& Reg = GetNodeRegistry();
	auto It = std::find_if(Reg.begin(), Reg.end(),
		[](const FNodeRegistration& E)
		{
			return std::string(E.TypeName) == "ModulationMatrix";
		});
	REQUIRE(It != Reg.end());
	REQUIRE(std::string(It->Category) == "Modulation");
	auto N = It->Make();
	REQUIRE(N);
	REQUIRE(std::string(N->GetTypeName()) == "ModulationMatrix");
}
