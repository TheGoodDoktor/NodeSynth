#include "dsp/Wavetable.h"

#include "dsp/internal/Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

namespace NodeSynth
{
	namespace
	{
		// RIFF / WAV chunk IDs as little-endian 32-bit integers. Compared
		// with std::memcmp on the raw 4-byte tag bytes — same effect, just
		// avoids a host-endianness assumption.
		constexpr char RiffTag[4] = { 'R', 'I', 'F', 'F' };
		constexpr char WaveTag[4] = { 'W', 'A', 'V', 'E' };
		constexpr char FmtTag[4]  = { 'f', 'm', 't', ' ' };
		constexpr char DataTag[4] = { 'd', 'a', 't', 'a' };

		// Read N bytes from the stream into a buffer; returns false on EOF
		// or stream failure.
		bool ReadBytes(std::ifstream& In, void* Dst, size_t N)
		{
			In.read(static_cast<char*>(Dst), static_cast<std::streamsize>(N));
			return In.good() || (In.eof() && In.gcount() == static_cast<std::streamsize>(N));
		}

		uint16_t ReadU16(std::ifstream& In)
		{
			uint8_t B[2] = {};
			ReadBytes(In, B, 2);
			return static_cast<uint16_t>(B[0]) | (static_cast<uint16_t>(B[1]) << 8);
		}

		uint32_t ReadU32(std::ifstream& In)
		{
			uint8_t B[4] = {};
			ReadBytes(In, B, 4);
			return static_cast<uint32_t>(B[0])
				| (static_cast<uint32_t>(B[1]) << 8)
				| (static_cast<uint32_t>(B[2]) << 16)
				| (static_cast<uint32_t>(B[3]) << 24);
		}

		void WriteU16(std::ofstream& Out, uint16_t V)
		{
			const uint8_t B[2] = {
				static_cast<uint8_t>(V & 0xFF),
				static_cast<uint8_t>((V >> 8) & 0xFF),
			};
			Out.write(reinterpret_cast<const char*>(B), 2);
		}

		void WriteU32(std::ofstream& Out, uint32_t V)
		{
			const uint8_t B[4] = {
				static_cast<uint8_t>(V & 0xFF),
				static_cast<uint8_t>((V >> 8) & 0xFF),
				static_cast<uint8_t>((V >> 16) & 0xFF),
				static_cast<uint8_t>((V >> 24) & 0xFF),
			};
			Out.write(reinterpret_cast<const char*>(B), 4);
		}

		// Decode a PCM data chunk into normalised float [-1, 1]. Stereo is
		// summed to mono (average L/R). Returns the resulting interleaved
		// mono samples; size = NumSamplesMono.
		std::vector<float> DecodePcm(
			const std::vector<uint8_t>& Raw,
			uint16_t Format,        // 1 = PCM int, 3 = IEEE float
			uint16_t BitsPerSample,
			uint16_t NumChannels)
		{
			const uint32_t BytesPerSample = static_cast<uint32_t>(BitsPerSample) / 8u;
			const uint32_t FrameStride = BytesPerSample * NumChannels;
			if (FrameStride == 0)
			{
				return {};
			}
			const size_t NumFrames = Raw.size() / FrameStride;
			std::vector<float> Out;
			Out.reserve(NumFrames);

			auto DecodeOne = [&](const uint8_t* P) -> float
			{
				if (Format == 3 && BitsPerSample == 32)
				{
					float V = 0.0f;
					std::memcpy(&V, P, 4);
					return V;
				}
				if (Format == 1)
				{
					if (BitsPerSample == 16)
					{
						const int16_t I = static_cast<int16_t>(
							static_cast<uint16_t>(P[0]) | (static_cast<uint16_t>(P[1]) << 8));
						return static_cast<float>(I) / 32768.0f;
					}
					if (BitsPerSample == 24)
					{
						// 24-bit signed little-endian; sign-extend.
						int32_t I = static_cast<int32_t>(P[0])
							| (static_cast<int32_t>(P[1]) << 8)
							| (static_cast<int32_t>(P[2]) << 16);
						if (I & 0x00800000) { I |= 0xFF000000; }
						return static_cast<float>(I) / 8388608.0f;
					}
					if (BitsPerSample == 32)
					{
						const int32_t I = static_cast<int32_t>(P[0])
							| (static_cast<int32_t>(P[1]) << 8)
							| (static_cast<int32_t>(P[2]) << 16)
							| (static_cast<int32_t>(P[3]) << 24);
						return static_cast<float>(I) / 2147483648.0f;
					}
				}
				return 0.0f;
			};

			for (size_t I = 0; I < NumFrames; ++I)
			{
				const uint8_t* Base = Raw.data() + I * FrameStride;
				if (NumChannels == 1)
				{
					Out.push_back(DecodeOne(Base));
				}
				else
				{
					float Sum = 0.0f;
					for (uint16_t Ch = 0; Ch < NumChannels; ++Ch)
					{
						Sum += DecodeOne(Base + Ch * BytesPerSample);
					}
					Out.push_back(Sum / static_cast<float>(NumChannels));
				}
			}
			return Out;
		}

		// Normalise a buffer's peak amplitude to 1.0. No-op if the buffer
		// is silent.
		void NormalizePeak(std::vector<float>& Buf)
		{
			float Peak = 0.0f;
			for (const float V : Buf)
			{
				const float A = std::fabs(V);
				if (A > Peak) { Peak = A; }
			}
			if (Peak <= 1e-9f) { return; }
			const float Inv = 1.0f / Peak;
			for (float& V : Buf) { V *= Inv; }
		}
	}

	void BuildFrameMips(FWavetableFrame& Frame)
	{
		const std::vector<float>& Source = Frame.Mips[0];
		const std::size_t N = FWavetableData::FrameSize;
		if (Source.size() != N)
		{
			return;
		}

		// FFT the source frame once.
		std::vector<std::complex<float>> Spectrum(N);
		for (std::size_t I = 0; I < N; ++I)
		{
			Spectrum[I] = std::complex<float>(Source[I], 0.0f);
		}
		Internal::Fft(Spectrum.data(), N, +1);

		// Mip 0 is the original — copy unchanged. Building it via
		// FFT round-trip would introduce small rounding errors and lose
		// the bit-identical-passthrough property at low pitches.
		// (Mip 0 is the source slot; it's already populated.)

		// For each higher mip, copy the spectrum, zero out bins above
		// the mip's harmonic limit (and their conjugate mirrors), then
		// inverse-FFT. The result has at most NumHarmonics(M) non-zero
		// frequency components, so playing it at frequencies up to
		// SampleRate/(2 * NumHarmonics(M)) won't alias.
		std::vector<std::complex<float>> Y(N);
		for (uint32_t M = 1; M < FWavetableFrame::NumMips; ++M)
		{
			Y = Spectrum;
			const std::size_t MaxBin = FWavetableData::NumHarmonics(M);
			// Zero bins (MaxBin+1 .. N-MaxBin-1). Keeps DC (bin 0),
			// harmonics 1..MaxBin, and their negative-frequency mirrors.
			for (std::size_t Bin = MaxBin + 1; Bin + MaxBin < N; ++Bin)
			{
				Y[Bin] = std::complex<float>(0.0f, 0.0f);
			}
			Internal::Fft(Y.data(), N, -1);
			Frame.Mips[M].resize(N);
			const float Inv = 1.0f / static_cast<float>(N);
			for (std::size_t I = 0; I < N; ++I)
			{
				Frame.Mips[M][I] = Y[I].real() * Inv;
			}
		}
	}

	void BuildWavetableMips(FWavetableData& Data)
	{
		for (FWavetableFrame& F : Data.Frames)
		{
			BuildFrameMips(F);
		}
	}

	std::shared_ptr<FWavetableData> LoadWavetable(const std::filesystem::path& Path)
	{
		std::ifstream In(Path, std::ios::binary);
		if (!In.is_open())
		{
			std::fprintf(stderr, "LoadWavetable: cannot open %s\n",
				Path.string().c_str());
			return nullptr;
		}

		// RIFF header.
		char Tag[4];
		if (!ReadBytes(In, Tag, 4) || std::memcmp(Tag, RiffTag, 4) != 0)
		{
			std::fprintf(stderr, "LoadWavetable: %s is not a RIFF file\n",
				Path.string().c_str());
			return nullptr;
		}
		(void)ReadU32(In);  // riff chunk size — ignored
		if (!ReadBytes(In, Tag, 4) || std::memcmp(Tag, WaveTag, 4) != 0)
		{
			std::fprintf(stderr, "LoadWavetable: %s is not a WAVE file\n",
				Path.string().c_str());
			return nullptr;
		}

		// Walk sub-chunks looking for fmt + data. Skip any others
		// (LIST/INFO/clm/uhWT/etc) — Serum metadata is intentionally
		// ignored per the v1 plan.
		uint16_t Format = 0;
		uint16_t NumChannels = 0;
		uint16_t BitsPerSample = 0;
		uint32_t SampleRate = 0;
		std::vector<uint8_t> RawData;
		bool bHaveFmt = false;
		bool bHaveData = false;

		while (In)
		{
			if (!ReadBytes(In, Tag, 4))
			{
				break;
			}
			const uint32_t ChunkSize = ReadU32(In);
			if (std::memcmp(Tag, FmtTag, 4) == 0)
			{
				Format = ReadU16(In);
				NumChannels = ReadU16(In);
				SampleRate = ReadU32(In);
				(void)ReadU32(In);  // byte rate
				(void)ReadU16(In);  // block align
				BitsPerSample = ReadU16(In);
				// Skip any extra fmt bytes (extensible WAVE etc).
				const uint32_t Consumed = 16;
				if (ChunkSize > Consumed)
				{
					In.seekg(ChunkSize - Consumed, std::ios::cur);
				}
				// fmt chunks are even-padded to a 2-byte boundary.
				if ((ChunkSize & 1u) != 0u)
				{
					In.seekg(1, std::ios::cur);
				}
				bHaveFmt = true;
			}
			else if (std::memcmp(Tag, DataTag, 4) == 0)
			{
				RawData.resize(ChunkSize);
				if (ChunkSize > 0)
				{
					In.read(reinterpret_cast<char*>(RawData.data()), ChunkSize);
				}
				if ((ChunkSize & 1u) != 0u)
				{
					In.seekg(1, std::ios::cur);
				}
				bHaveData = true;
			}
			else
			{
				// Unknown chunk; skip body + padding.
				In.seekg(ChunkSize, std::ios::cur);
				if ((ChunkSize & 1u) != 0u)
				{
					In.seekg(1, std::ios::cur);
				}
			}
		}

		if (!bHaveFmt || !bHaveData)
		{
			std::fprintf(stderr, "LoadWavetable: %s missing fmt or data chunk\n",
				Path.string().c_str());
			return nullptr;
		}
		if (Format != 1 && Format != 3)
		{
			std::fprintf(stderr, "LoadWavetable: %s unsupported format %u\n",
				Path.string().c_str(), Format);
			return nullptr;
		}
		if (NumChannels < 1 || NumChannels > 2)
		{
			std::fprintf(stderr, "LoadWavetable: %s unsupported channel count %u\n",
				Path.string().c_str(), NumChannels);
			return nullptr;
		}
		if (BitsPerSample != 16 && BitsPerSample != 24 && BitsPerSample != 32)
		{
			std::fprintf(stderr, "LoadWavetable: %s unsupported bit depth %u\n",
				Path.string().c_str(), BitsPerSample);
			return nullptr;
		}
		(void)SampleRate;  // unused for wavetables — frame size determines pitch

		std::vector<float> Mono = DecodePcm(RawData, Format, BitsPerSample, NumChannels);
		if (Mono.empty())
		{
			std::fprintf(stderr, "LoadWavetable: %s decoded empty\n",
				Path.string().c_str());
			return nullptr;
		}

		// Total length must be a multiple of FrameSize for v0. WT.3 will
		// add FFT-based upsampling to handle 256 / 512 / 1024 frame sizes.
		if ((Mono.size() % FWavetableData::FrameSize) != 0)
		{
			std::fprintf(stderr,
				"LoadWavetable: %s length %zu is not a multiple of %u "
				"(non-2048-frame WTs not yet supported)\n",
				Path.string().c_str(), Mono.size(), FWavetableData::FrameSize);
			return nullptr;
		}
		const size_t NumFrames = Mono.size() / FWavetableData::FrameSize;
		if (NumFrames == 0)
		{
			return nullptr;
		}

		// Normalise once across the whole file (preserves relative loudness
		// between frames; per-frame normalisation would equalise quiet
		// frames against loud ones and distort the morph).
		NormalizePeak(Mono);

		auto Data = std::make_shared<FWavetableData>();
		Data->Name = Path.stem().string();
		Data->Frames.resize(NumFrames);
		for (size_t F = 0; F < NumFrames; ++F)
		{
			const float* Src = Mono.data() + F * FWavetableData::FrameSize;
			Data->Frames[F].Mips[0].assign(Src, Src + FWavetableData::FrameSize);
		}
		// Build the bandlimited mips so the oscillator can pick the right
		// one per block based on playback frequency.
		BuildWavetableMips(*Data);
		return Data;
	}

	bool SaveWavetable(const FWavetableData& Data, const std::filesystem::path& Path)
	{
		if (Data.Frames.empty())
		{
			return false;
		}
		// Sanity: every frame must have its Mip 0 populated at the
		// canonical size. Mips 1..7 are derived on load — we don't
		// persist them.
		for (const FWavetableFrame& F : Data.Frames)
		{
			if (F.Mips[0].size() != FWavetableData::FrameSize) { return false; }
		}

		std::error_code Ec;
		std::filesystem::create_directories(Path.parent_path(), Ec);

		std::ofstream Out(Path, std::ios::binary | std::ios::trunc);
		if (!Out.is_open()) { return false; }

		const uint32_t TotalSamples =
			static_cast<uint32_t>(Data.Frames.size()) * FWavetableData::FrameSize;
		const uint16_t NumChannels = 1;
		const uint16_t BitsPerSample = 32;
		const uint16_t Format = 3;  // IEEE float
		const uint32_t SampleRate = 48000;
		const uint16_t BlockAlign = NumChannels * (BitsPerSample / 8);
		const uint32_t ByteRate = SampleRate * BlockAlign;
		const uint32_t DataBytes = TotalSamples * BlockAlign;
		const uint32_t FmtChunkSize = 16;
		const uint32_t RiffSize = 4 /*WAVE*/ + (8 + FmtChunkSize) + (8 + DataBytes);

		Out.write(RiffTag, 4);
		WriteU32(Out, RiffSize);
		Out.write(WaveTag, 4);

		Out.write(FmtTag, 4);
		WriteU32(Out, FmtChunkSize);
		WriteU16(Out, Format);
		WriteU16(Out, NumChannels);
		WriteU32(Out, SampleRate);
		WriteU32(Out, ByteRate);
		WriteU16(Out, BlockAlign);
		WriteU16(Out, BitsPerSample);

		Out.write(DataTag, 4);
		WriteU32(Out, DataBytes);
		for (const FWavetableFrame& F : Data.Frames)
		{
			const std::vector<float>& Samples = F.Mips[0];
			Out.write(reinterpret_cast<const char*>(Samples.data()),
				static_cast<std::streamsize>(Samples.size() * sizeof(float)));
		}
		return Out.good();
	}
}
