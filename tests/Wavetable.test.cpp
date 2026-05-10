// Tests for FWavetableData / LoadWavetable / FWavetableOscillator.
// Covers WT.1 (data + WAV loader), WT.2 (oscillator node), and
// WT.3 (mip-mapped anti-aliasing).

#include "dsp/Node.h"
#include "dsp/Wavetable.h"
#include "dsp/WavetableOscillator.h"
#include "dsp/internal/Fft.h"
#include "ui/NodeRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <complex>
#include <filesystem>
#include <vector>

using namespace NodeSynth;

namespace
{
	constexpr float Pi = 3.14159265358979f;

	// Build a wavetable with the four basic shapes as 4 separate frames.
	// Each frame is one period long at FrameSize samples. Mip 0 only —
	// callers run BuildWavetableMips if they need the bandlimited mips.
	FWavetableData MakeBasicShapes()
	{
		FWavetableData D;
		D.Name = "BasicShapes";
		D.Frames.resize(4);
		for (auto& F : D.Frames) { F.Mips[0].resize(FWavetableData::FrameSize); }
		const uint32_t N = FWavetableData::FrameSize;
		for (uint32_t I = 0; I < N; ++I)
		{
			const float T = static_cast<float>(I) / static_cast<float>(N);
			D.Frames[0].Mips[0][I] = std::sin(T * 2.0f * Pi);
			const float Tri = (T < 0.25f) ? 4.0f * T
				: (T < 0.75f) ? 2.0f - 4.0f * T
				: -4.0f + 4.0f * T;
			D.Frames[1].Mips[0][I] = Tri;
			D.Frames[2].Mips[0][I] = 2.0f * T - 1.0f;            // saw
			D.Frames[3].Mips[0][I] = (T < 0.5f) ? 1.0f : -1.0f;  // square
		}
		return D;
	}

	// Use a per-test temp directory under build/Wavetable.test.tmp so the
	// CI runner's working tree stays clean.
	std::filesystem::path TempDir()
	{
		const std::filesystem::path Root =
			std::filesystem::temp_directory_path() / "nodesynth-wt-tests";
		std::filesystem::create_directories(Root);
		return Root;
	}
}

TEST_CASE("Wavetable round-trip preserves frame samples", "[wavetable]")
{
	const FWavetableData Original = MakeBasicShapes();
	const auto Path = TempDir() / "round_trip.wav";
	REQUIRE(SaveWavetable(Original, Path));

	const auto Loaded = LoadWavetable(Path);
	REQUIRE(Loaded);
	REQUIRE(Loaded->NumFrames() == 4);
	REQUIRE(Loaded->Frames[0].Mips[0].size() == FWavetableData::FrameSize);

	// Saved file is normalised by the loader so peak == 1; the original's
	// peak is also 1 (saw and square hit ±1 exactly), so loaded == original
	// (Mip 0) to within float-write/read precision.
	for (size_t F = 0; F < Original.Frames.size(); ++F)
	{
		for (size_t I = 0; I < FWavetableData::FrameSize; ++I)
		{
			REQUIRE_THAT(Loaded->Frames[F].Mips[0][I],
				Catch::Matchers::WithinAbs(Original.Frames[F].Mips[0][I], 1e-6f));
		}
	}
}

TEST_CASE("Wavetable rejects bad files", "[wavetable]")
{
	// Frame Mip 0 length not equal to FrameSize.
	{
		FWavetableData D;
		FWavetableFrame F;
		F.Mips[0].assign(2049, 0.0f);  // odd length — SaveWavetable should reject
		D.Frames.push_back(std::move(F));
		const auto P = TempDir() / "bad_len.wav";
		REQUIRE_FALSE(SaveWavetable(D, P));
	}
	// Path that doesn't exist.
	REQUIRE(LoadWavetable(TempDir() / "definitely_not_here.wav") == nullptr);
}

TEST_CASE("Wavetable Name is set from filename", "[wavetable]")
{
	const auto Path = TempDir() / "MyTable.wav";
	REQUIRE(SaveWavetable(MakeBasicShapes(), Path));
	const auto Loaded = LoadWavetable(Path);
	REQUIRE(Loaded);
	REQUIRE(Loaded->Name == "MyTable");
}

TEST_CASE("WavetableOscillator with no table produces silence", "[wavetable]")
{
	FWavetableOscillator Osc;
	Osc.Prepare(48000.0);
	std::array<float, BlockSize> FreqBuf{};
	for (auto& V : FreqBuf) { V = 220.0f; }
	Osc.SetInputBuffer(FWavetableOscillator::Input_Frequency, FreqBuf.data());

	FProcessContext Ctx;
	Osc.Process(Ctx);

	float* Out = Osc.GetOutputBuffer(0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE(Out[I] == 0.0f);
	}
}

TEST_CASE("WavetableOscillator plays a single-frame saw at expected pitch", "[wavetable]")
{
	// Single-frame saw — at 220 Hz on a 48 kHz host, the phase wraps every
	// ~218 samples. Our block is 64 samples — each block sees about 0.29
	// of a period, so we sweep monotonically up (with a few wraps per
	// long run).
	FWavetableData Saw;
	Saw.Frames.resize(1);
	Saw.Frames[0].Mips[0].resize(FWavetableData::FrameSize);
	for (uint32_t I = 0; I < FWavetableData::FrameSize; ++I)
	{
		const float T = static_cast<float>(I) / FWavetableData::FrameSize;
		Saw.Frames[0].Mips[0][I] = 2.0f * T - 1.0f;  // -1 .. +1 ramp
	}
	const auto Path = TempDir() / "saw_single.wav";
	REQUIRE(SaveWavetable(Saw, Path));

	FWavetableOscillator Osc;
	Osc.SetParamString(FWavetableOscillator::Param_Wavetable, Path.string());
	Osc.SetParamValue(FWavetableOscillator::Param_Amplitude, 1.0f);
	Osc.Prepare(48000.0);

	// Drive a constant 220 Hz frequency.
	std::array<float, BlockSize> FreqBuf{};
	for (auto& V : FreqBuf) { V = 220.0f; }
	Osc.SetInputBuffer(FWavetableOscillator::Input_Frequency, FreqBuf.data());

	FProcessContext Ctx;
	bool bAnyNonZero = false;
	float MinV = 1e9f;
	float MaxV = -1e9f;
	for (uint32_t Block = 0; Block < 16; ++Block)
	{
		Osc.Process(Ctx);
		float* Out = Osc.GetOutputBuffer(0);
		for (uint32_t I = 0; I < BlockSize; ++I)
		{
			if (Out[I] != 0.0f) { bAnyNonZero = true; }
			MinV = std::min(MinV, Out[I]);
			MaxV = std::max(MaxV, Out[I]);
		}
	}
	REQUIRE(bAnyNonZero);
	// 16 blocks @ 64 samples = 1024 samples ≈ 4.7 saw periods. Should hit
	// near both ±1 extremes.
	REQUIRE(MaxV > 0.9f);
	REQUIRE(MinV < -0.9f);
}

TEST_CASE("WavetableOscillator Position param picks frames", "[wavetable]")
{
	const FWavetableData Original = MakeBasicShapes();
	const auto Path = TempDir() / "shapes.wav";
	REQUIRE(SaveWavetable(Original, Path));

	FWavetableOscillator Osc;
	Osc.SetParamString(FWavetableOscillator::Param_Wavetable, Path.string());
	Osc.SetParamValue(FWavetableOscillator::Param_Amplitude, 1.0f);
	Osc.SetParamValue(FWavetableOscillator::Param_Phase, 0.25f);  // peak of sine
	Osc.Prepare(48000.0);

	// Use a very low frequency so phase barely advances within a block —
	// the first sample of each block is approximately the frame value at
	// phase=0.25 (which is sin(π/2) = 1 for the sine frame).
	std::array<float, BlockSize> FreqBuf{};
	for (auto& V : FreqBuf) { V = 0.0f; }
	Osc.SetInputBuffer(FWavetableOscillator::Input_Frequency, FreqBuf.data());

	auto FirstSampleAtPosition = [&](float Pos) -> float
	{
		Osc.SetParamValue(FWavetableOscillator::Param_Position, Pos);
		Osc.Prepare(48000.0);  // resets phase to PhaseOffset
		FProcessContext Ctx;
		Osc.Process(Ctx);
		return Osc.GetOutputBuffer(0)[0];
	};

	// At Position=0, frame 0 (sine at phase 0.25) ≈ +1.
	REQUIRE_THAT(FirstSampleAtPosition(0.0f),
		Catch::Matchers::WithinAbs(1.0f, 1e-3f));
	// At Position=2/3 (frame index 2.0, exact), saw at phase 0.25 = -0.5.
	REQUIRE_THAT(FirstSampleAtPosition(2.0f / 3.0f),
		Catch::Matchers::WithinAbs(-0.5f, 1e-2f));
	// At Position=1.0, frame 3 (square at phase 0.25 < 0.5) = +1.
	REQUIRE_THAT(FirstSampleAtPosition(1.0f),
		Catch::Matchers::WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("WavetableOscillator Position Control input overrides param", "[wavetable]")
{
	const auto Path = TempDir() / "shapes2.wav";
	REQUIRE(SaveWavetable(MakeBasicShapes(), Path));

	FWavetableOscillator Osc;
	Osc.SetParamString(FWavetableOscillator::Param_Wavetable, Path.string());
	Osc.SetParamValue(FWavetableOscillator::Param_Amplitude, 1.0f);
	Osc.SetParamValue(FWavetableOscillator::Param_Phase, 0.25f);
	Osc.SetParamValue(FWavetableOscillator::Param_Position, 0.0f);  // sine
	Osc.Prepare(48000.0);

	std::array<float, BlockSize> FreqBuf{};
	std::array<float, BlockSize> PosBuf{};
	for (auto& V : FreqBuf) { V = 0.0f; }
	for (auto& V : PosBuf)  { V = 1.0f; }  // override to last frame (square)
	Osc.SetInputBuffer(FWavetableOscillator::Input_Frequency, FreqBuf.data());
	Osc.SetInputBuffer(FWavetableOscillator::Input_Position,  PosBuf.data());

	FProcessContext Ctx;
	Osc.Process(Ctx);

	// Param said sine (0); Control buffer overrode to square (1) → +1 at phase 0.25.
	REQUIRE_THAT(Osc.GetOutputBuffer(0)[0],
		Catch::Matchers::WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("WavetableOscillator clones share wavetable data", "[wavetable]")
{
	const auto Path = TempDir() / "clone.wav";
	REQUIRE(SaveWavetable(MakeBasicShapes(), Path));

	auto Master = std::make_shared<FWavetableOscillator>();
	Master->SetParamString(FWavetableOscillator::Param_Wavetable, Path.string());
	Master->SetParamValue(FWavetableOscillator::Param_Position, 0.5f);
	Master->SetParamValue(FWavetableOscillator::Param_Amplitude, 0.7f);

	std::shared_ptr<INode> Cloned = Master->Clone();
	REQUIRE(Cloned);
	auto* C = dynamic_cast<FWavetableOscillator*>(Cloned.get());
	REQUIRE(C);

	// Param values transferred.
	REQUIRE_THAT(C->GetParamValue(FWavetableOscillator::Param_Position),
		Catch::Matchers::WithinAbs(0.5f, 1e-6f));
	REQUIRE_THAT(C->GetParamValue(FWavetableOscillator::Param_Amplitude),
		Catch::Matchers::WithinAbs(0.7f, 1e-6f));

	// Path and underlying data both preserved.
	REQUIRE(C->GetParamString(FWavetableOscillator::Param_Wavetable) == Path.string());

	// Both should produce identical output for the same Frequency/Position
	// inputs.
	Master->Prepare(48000.0);
	C->Prepare(48000.0);

	std::array<float, BlockSize> FreqBuf{};
	for (auto& V : FreqBuf) { V = 220.0f; }
	Master->SetInputBuffer(FWavetableOscillator::Input_Frequency, FreqBuf.data());
	C->SetInputBuffer(FWavetableOscillator::Input_Frequency, FreqBuf.data());

	FProcessContext Ctx;
	Master->Process(Ctx);
	C->Process(Ctx);

	const float* OutA = Master->GetOutputBuffer(0);
	const float* OutB = C->GetOutputBuffer(0);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE_THAT(OutA[I], Catch::Matchers::WithinAbs(OutB[I], 1e-6f));
	}
}

TEST_CASE("FFT round-trip recovers the original signal", "[wavetable][fft]")
{
	// Forward then inverse FFT should reproduce the input (modulo float
	// precision). Inverse leaves the result unscaled — divide by N.
	constexpr std::size_t N = 1024;
	std::vector<std::complex<float>> X(N);
	for (std::size_t I = 0; I < N; ++I)
	{
		const float T = static_cast<float>(I) / static_cast<float>(N);
		const float Sample = std::sin(T * 2.0f * Pi)
			+ 0.5f * std::sin(T * 8.0f * Pi);
		X[I] = std::complex<float>(Sample, 0.0f);
	}
	const std::vector<std::complex<float>> Original = X;

	Internal::Fft(X.data(), N, +1);
	Internal::Fft(X.data(), N, -1);

	const float Inv = 1.0f / static_cast<float>(N);
	for (std::size_t I = 0; I < N; ++I)
	{
		REQUIRE_THAT(X[I].real() * Inv,
			Catch::Matchers::WithinAbs(Original[I].real(), 1e-3f));
		REQUIRE_THAT(X[I].imag() * Inv,
			Catch::Matchers::WithinAbs(Original[I].imag(), 1e-3f));
	}
}

TEST_CASE("Built mip retains only the allowed harmonics", "[wavetable][fft]")
{
	// Construct a frame containing the first 32 harmonics of a saw.
	// After mip building, mip M should retain at most NumHarmonics(M)
	// harmonics — energy in higher bins must be ~0.
	FWavetableFrame F;
	F.Mips[0].assign(FWavetableData::FrameSize, 0.0f);
	const uint32_t N = FWavetableData::FrameSize;
	for (uint32_t H = 1; H <= 32; ++H)
	{
		const float Amp = 1.0f / static_cast<float>(H);
		for (uint32_t I = 0; I < N; ++I)
		{
			const float T = static_cast<float>(I) / static_cast<float>(N);
			F.Mips[0][I] += Amp * std::sin(T * 2.0f * Pi * static_cast<float>(H));
		}
	}
	BuildFrameMips(F);

	// Mip 5 is allowed up to 32 harmonics — it should look identical to
	// Mip 0 (we only put 32 harmonics in to begin with). Mip 6 is allowed
	// 16, so harmonics 17..32 should be ~zero.
	auto SpectrumOfMip = [&](uint32_t Mip) -> std::vector<float>
	{
		std::vector<std::complex<float>> X(N);
		for (uint32_t I = 0; I < N; ++I)
		{
			X[I] = std::complex<float>(F.Mips[Mip][I], 0.0f);
		}
		Internal::Fft(X.data(), N, +1);
		std::vector<float> Mag(N / 2);
		for (uint32_t Bin = 0; Bin < N / 2; ++Bin)
		{
			Mag[Bin] = std::abs(X[Bin]);
		}
		return Mag;
	};

	const std::vector<float> Mag6 = SpectrumOfMip(6);
	const std::vector<float> Mag7 = SpectrumOfMip(7);

	// Reference: bin 1 (fundamental) of the original should still be
	// large — ~N/2 * amplitude ~= 1024 * 1.0 = 1024.
	REQUIRE(Mag6[1] > 100.0f);

	// Mip 6 keeps 16 harmonics. Bins 17..32 (originally non-zero in the
	// source) must be approximately zero now.
	for (uint32_t Bin = 17; Bin <= 32; ++Bin)
	{
		REQUIRE(Mag6[Bin] < 1e-2f);
	}

	// Mip 7 keeps 8 harmonics. Bins 9..32 must be ~zero.
	for (uint32_t Bin = 9; Bin <= 32; ++Bin)
	{
		REQUIRE(Mag7[Bin] < 1e-2f);
	}

	// And the fundamental in mip 7 should still be roughly the same as
	// in the source — bandlimiting only drops higher harmonics, not the
	// fundamental.
	REQUIRE(Mag7[1] > 100.0f);
}

TEST_CASE("Wavetable mips are built on load", "[wavetable]")
{
	// Round-trip: save Mip 0 only, load — the loader runs the mip
	// generator and Mip 1..7 should be populated.
	const auto Path = TempDir() / "with_mips.wav";
	REQUIRE(SaveWavetable(MakeBasicShapes(), Path));
	const auto Loaded = LoadWavetable(Path);
	REQUIRE(Loaded);
	for (const FWavetableFrame& F : Loaded->Frames)
	{
		for (uint32_t M = 0; M < FWavetableFrame::NumMips; ++M)
		{
			REQUIRE(F.Mips[M].size() == FWavetableData::FrameSize);
		}
	}
}

TEST_CASE("WavetableOscillator anti-aliases at high pitch", "[wavetable]")
{
	// Build a saw-rich wavetable, run the oscillator at a high pitch
	// where mip 0 would alias hard, and assert the captured output's
	// spectral energy above Nyquist's worth of legal harmonics is
	// suppressed.
	//
	// At F=2200 Hz (≈C7) and SR=48000, only 24000/2200 ≈ 10 harmonics
	// fit under Nyquist. Without mips the saw's harmonics 11..1024
	// would all alias back into audible range. With mips selected by
	// SelectMip the oscillator should pick mip ~6 (16 harmonics — close
	// to the 10 legal ones), meaning energy above bin ~10 should be
	// dramatically lower than energy at the fundamental.
	FWavetableData Saw;
	Saw.Frames.resize(1);
	Saw.Frames[0].Mips[0].assign(FWavetableData::FrameSize, 0.0f);
	const uint32_t N = FWavetableData::FrameSize;
	for (uint32_t H = 1; H <= 256; ++H)
	{
		const float Amp = 1.0f / static_cast<float>(H);
		for (uint32_t I = 0; I < N; ++I)
		{
			const float T = static_cast<float>(I) / static_cast<float>(N);
			Saw.Frames[0].Mips[0][I] +=
				Amp * std::sin(T * 2.0f * Pi * static_cast<float>(H));
		}
	}
	BuildWavetableMips(Saw);

	const auto Path = TempDir() / "alias_test_input.wav";
	REQUIRE(SaveWavetable(Saw, Path));

	FWavetableOscillator Osc;
	Osc.SetParamString(FWavetableOscillator::Param_Wavetable, Path.string());
	Osc.SetParamValue(FWavetableOscillator::Param_Amplitude, 1.0f);
	Osc.Prepare(48000.0);

	std::array<float, BlockSize> FreqBuf{};
	for (auto& V : FreqBuf) { V = 2200.0f; }
	Osc.SetInputBuffer(FWavetableOscillator::Input_Frequency, FreqBuf.data());

	// Capture 8192 samples. The oscillator's mip selection runs at
	// block boundaries — same mip throughout this run.
	constexpr uint32_t CaptureLen = 8192;
	std::vector<float> Captured(CaptureLen, 0.0f);
	FProcessContext Ctx;
	for (uint32_t Block = 0; Block * BlockSize < CaptureLen; ++Block)
	{
		Osc.Process(Ctx);
		const float* Out = Osc.GetOutputBuffer(0);
		for (uint32_t I = 0; I < BlockSize; ++I)
		{
			Captured[Block * BlockSize + I] = Out[I];
		}
	}

	// FFT the capture and look for energy above 12 kHz (well above the
	// 10 harmonics × 2200 Hz = 22 kHz that would be legal). Without
	// mip-mapped AA, alias content here would be substantial.
	std::vector<std::complex<float>> X(CaptureLen);
	for (uint32_t I = 0; I < CaptureLen; ++I)
	{
		X[I] = std::complex<float>(Captured[I], 0.0f);
	}
	Internal::Fft(X.data(), CaptureLen, +1);

	const float HzPerBin = 48000.0f / static_cast<float>(CaptureLen);
	const uint32_t FundamentalBin = static_cast<uint32_t>(2200.0f / HzPerBin + 0.5f);
	const float FundamentalEnergy = std::abs(X[FundamentalBin]);
	REQUIRE(FundamentalEnergy > 1.0f);  // sanity: the saw is loud

	// The oscillator picks the mip whose harmonic budget is closest to
	// Nyquist/freq = ~11. Mip 6 (16 harmonics) is the cheapest fit.
	// Its highest harmonic at 2200 Hz lives at 16 * 2200 = 35.2 kHz,
	// which folds back via aliasing if undersampled — but our mips
	// truncate the SOURCE harmonics, so the only energy that exists is
	// at bin*Fundamental for harmonic indices the mip retains.
	// Anything above 16 * 2200 = 35.2 kHz, modulo Nyquist folding, must
	// not exceed -50 dB relative to the fundamental.
	const float MaxAllowedAliasBin = 16.0f * 2200.0f / HzPerBin;
	float AliasPeak = 0.0f;
	for (uint32_t Bin = static_cast<uint32_t>(MaxAllowedAliasBin) + 4;
		Bin < CaptureLen / 2; ++Bin)
	{
		AliasPeak = std::max(AliasPeak, std::abs(X[Bin]));
	}
	const float AliasDb =
		20.0f * std::log10(std::max(AliasPeak / FundamentalEnergy, 1e-12f));
	INFO("Alias peak relative to fundamental: " << AliasDb << " dB");
	REQUIRE(AliasDb < -40.0f);
}

TEST_CASE("WavetableOscillator registered in palette under Sources", "[wavetable]")
{
	const auto& Reg = GetNodeRegistry();
	auto It = std::find_if(Reg.begin(), Reg.end(),
		[](const FNodeRegistration& E)
		{
			return std::string(E.TypeName) == "WavetableOscillator";
		});
	REQUIRE(It != Reg.end());
	REQUIRE(std::string(It->Category) == "Sources");

	// Registry should also be able to construct one.
	auto N = It->Make();
	REQUIRE(N);
	REQUIRE(std::string(N->GetTypeName()) == "WavetableOscillator");
}
