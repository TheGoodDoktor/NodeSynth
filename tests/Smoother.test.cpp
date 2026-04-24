#include "dsp/Smoother.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using NodeSynth::FOnePoleSmoother;

TEST_CASE("FOnePoleSmoother converges toward the target", "[smoother]")
{
	FOnePoleSmoother Smoother;
	Smoother.Prepare(48000.0, 5.0f);
	Smoother.Reset(0.0f);
	Smoother.SetTarget(1.0f);

	// After many ticks the smoother should be indistinguishable from the target.
	for (int I = 0; I < 48000; ++I)
	{
		Smoother.Tick();
	}
	REQUIRE_THAT(Smoother.GetCurrent(), Catch::Matchers::WithinAbs(1.0f, 1e-4f));
}

TEST_CASE("FOnePoleSmoother without ticks stays put", "[smoother]")
{
	FOnePoleSmoother Smoother;
	Smoother.Prepare(48000.0, 5.0f);
	Smoother.Reset(0.5f);
	Smoother.SetTarget(1.0f);
	REQUIRE_THAT(Smoother.GetCurrent(), Catch::Matchers::WithinAbs(0.5f, 1e-6f));
}
