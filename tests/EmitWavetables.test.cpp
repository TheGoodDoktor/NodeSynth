// Hidden-by-default Catch2 test that writes the bundled wavetable .wav files
// out to disk. Tagged [.][wavetable-emit] so the standard `nodesynth_tests`
// run skips it; regenerate the bundled wavetables by selecting the tag
// explicitly:
//
//   ./build/Release/nodesynth_tests.exe "[wavetable-emit]"
//
// from the repo root. Output goes to ./wavetables/<Name>.wav. The resulting
// WAVs are committed to git — the runtime reads from the binary's bundled
// wavetables/ (copied at build time), not from this test, so the only
// purpose here is reproducibility of the wavetable content.

#include "dsp/Wavetable.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>

using namespace NodeSynth;

namespace
{
	constexpr float Pi = 3.14159265358979f;
	constexpr uint32_t N = FWavetableData::FrameSize;  // 2048

	// Four-frame morph: Sine → Triangle → Saw → Square. The simplest WT
	// you can ship; useful for verifying the playback chain works on a
	// fresh install.
	void EmitBasicShapes(const std::filesystem::path& Root)
	{
		FWavetableData D;
		D.Name = "BasicShapes";
		D.Frames.resize(4);
		for (auto& F : D.Frames) { F.Mips[0].resize(N); }

		for (uint32_t I = 0; I < N; ++I)
		{
			const float T = static_cast<float>(I) / static_cast<float>(N);
			D.Frames[0].Mips[0][I] = std::sin(T * 2.0f * Pi);
			const float Tri = (T < 0.25f) ? 4.0f * T
				: (T < 0.75f) ? 2.0f - 4.0f * T
				: -4.0f + 4.0f * T;
			D.Frames[1].Mips[0][I] = Tri;
			D.Frames[2].Mips[0][I] = 2.0f * T - 1.0f;
			D.Frames[3].Mips[0][I] = (T < 0.5f) ? 1.0f : -1.0f;
		}
		REQUIRE(SaveWavetable(D, Root / "BasicShapes.wav"));
	}

	// 32-frame FM bell. Frame F is a sine carrier ring-modulated with a
	// 3:1-ratio sine modulator at increasing depth. Position sweep walks
	// from a clean fundamental at F=0 to a metallic, harmonically-dense
	// timbre at F=31 — the classic Yamaha-DX-style bell morph.
	void EmitFMBell(const std::filesystem::path& Root)
	{
		FWavetableData D;
		D.Name = "FMBell";
		constexpr uint32_t NumFrames = 32;
		D.Frames.resize(NumFrames);

		for (uint32_t F = 0; F < NumFrames; ++F)
		{
			D.Frames[F].Mips[0].resize(N);
			// Modulation depth ramps 0..6 across the table. Values above ~4
			// are firmly in inharmonic-bell territory.
			const float Depth = 6.0f * static_cast<float>(F)
				/ static_cast<float>(NumFrames - 1);
			for (uint32_t I = 0; I < N; ++I)
			{
				const float T = static_cast<float>(I) / static_cast<float>(N);
				const float Modulator = std::sin(T * 2.0f * Pi * 3.0f);
				const float Phase = T * 2.0f * Pi + Depth * Modulator;
				D.Frames[F].Mips[0][I] = std::sin(Phase);
			}
		}
		REQUIRE(SaveWavetable(D, Root / "FMBell.wav"));
	}

	// 16-frame additive sweep. Frame F contains harmonics 1..(F+1) summed
	// with 1/n amplitude scaling — a saw built up one harmonic at a time.
	// Sweeping Position 0→1 morphs from a pure sine to a near-perfect saw.
	void EmitAdditiveSweep(const std::filesystem::path& Root)
	{
		FWavetableData D;
		D.Name = "AdditiveSweep";
		constexpr uint32_t NumFrames = 16;
		D.Frames.resize(NumFrames);

		for (uint32_t F = 0; F < NumFrames; ++F)
		{
			D.Frames[F].Mips[0].resize(N);
			const uint32_t MaxHarmonic = F + 1;
			for (uint32_t I = 0; I < N; ++I)
			{
				const float T = static_cast<float>(I) / static_cast<float>(N);
				float Sum = 0.0f;
				for (uint32_t H = 1; H <= MaxHarmonic; ++H)
				{
					Sum += std::sin(T * 2.0f * Pi * static_cast<float>(H))
						/ static_cast<float>(H);
				}
				D.Frames[F].Mips[0][I] = Sum;
			}
		}
		REQUIRE(SaveWavetable(D, Root / "AdditiveSweep.wav"));
	}
}

TEST_CASE("Emit bundled wavetables to ./wavetables/", "[.][wavetable-emit]")
{
	const std::filesystem::path Root =
		std::filesystem::current_path() / "wavetables";
	std::filesystem::create_directories(Root);

	EmitBasicShapes(Root);
	EmitFMBell(Root);
	EmitAdditiveSweep(Root);
}
