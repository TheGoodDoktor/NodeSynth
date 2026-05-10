#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace NodeSynth
{
	// One frame of a wavetable. Holds the original samples (Mip 0) plus 7
	// progressively bandlimited copies. Mip M has at most NumHarmonics(M)
	// non-zero spectral bins, so playing it at a high pitch can't produce
	// harmonics above Nyquist. The oscillator picks the right mip per
	// block based on the fundamental frequency.
	struct FWavetableFrame
	{
		// Mip 0 = full bandwidth (the file's original samples), mip 7 =
		// limited to 8 harmonics.
		static constexpr uint32_t NumMips = 8;
		std::array<std::vector<float>, NumMips> Mips;

		// Convenience accessors for Mip 0 (the canonical, full-bandwidth
		// frame) so callers that don't care about anti-aliasing can read
		// or write samples without indexing the array.
		const std::vector<float>& Samples() const { return Mips[0]; }
		std::vector<float>& Samples() { return Mips[0]; }
	};

	// Wavetable data: a stack of single-cycle waveforms (frames). The
	// FWavetableOscillator interpolates between adjacent frames based on
	// its Position param to produce evolving timbres. See
	// docs/PLAN-WAVETABLES.md.
	struct FWavetableData
	{
		// Canonical frame length. Files with smaller frame sizes are
		// rejected for now; future phases will add FFT-domain upsampling.
		static constexpr uint32_t FrameSize = 2048;
		static constexpr uint32_t NumMips = FWavetableFrame::NumMips;

		std::vector<FWavetableFrame> Frames;

		// Display name (file stem at load time). Surfaced by the UI; not
		// required for playback. Empty if built from a buffer.
		std::string Name;

		uint32_t NumFrames() const { return static_cast<uint32_t>(Frames.size()); }

		// How many harmonics mip M is allowed to retain. Mip 0 keeps all
		// FrameSize/2; each higher mip halves the count.
		static constexpr uint32_t NumHarmonics(uint32_t Mip)
		{
			return (FrameSize / 2u) >> Mip;
		}
	};

	// Build all 7 reduced-bandwidth mips for a frame whose Mip 0 is
	// already populated. Mip 0 is left untouched. Public so tests can
	// drive the mip generator without going through disk I/O.
	void BuildFrameMips(FWavetableFrame& Frame);

	// Run BuildFrameMips on every frame in a wavetable. Idempotent — safe
	// to call after editing Mip 0 of any frame.
	void BuildWavetableMips(FWavetableData& Data);

	// Load a wavetable from a PCM WAV file on disk. Mono (stereo is summed
	// to mono); supports 16-bit / 24-bit / 32-bit int and 32-bit float
	// formats. Total sample count must be an integer multiple of
	// FrameSize=2048; smaller-frame WTs are deferred. Returns nullptr on
	// parse / format / size failure. UI thread only — allocates, reads
	// from disk, runs FFT mip generation. Never call from the audio
	// callback.
	std::shared_ptr<FWavetableData> LoadWavetable(const std::filesystem::path& Path);

	// Write a wavetable as a 32-bit-float PCM WAV. Mono. Persists Mip 0
	// only — mips 1..7 are derived on load. Used by the bundled-wavetable
	// emitter test and unit tests' round-trip checks. Returns false on
	// filesystem failure.
	bool SaveWavetable(const FWavetableData& Data, const std::filesystem::path& Path);
}
