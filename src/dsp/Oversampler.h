#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "dsp/Node.h"

namespace NodeSynth
{
	// 2x oversampler — zero-stuff upsampler + windowed-sinc FIR + decimating
	// downsampler. The host node hands `Process` an input block of N samples,
	// an output buffer of N samples, and a callable that processes the
	// upsampled 2*N buffer in place at twice the sample rate. State (input
	// history) is kept across blocks for boundary continuity.
	//
	// The same FIR is shared between up- and down-sample paths (symmetric
	// design) — coefficients are computed in Prepare via Hamming-windowed
	// sinc at the half-band cutoff. For a 31-tap design this gives ~50 dB
	// stopband attenuation, comfortable for synth-grade use.
	//
	// CPU cost is ~2 * NumTaps multiply-adds per host sample (one pass
	// upsample + one pass downsample), totalling ~120 mac/sample for 31
	// taps. Negligible on modern CPUs.
	//
	// Latency: (NumTaps - 1) / 2 samples each direction at the 2x rate
	// = (NumTaps - 1) / 2 host samples total. For 31 taps that's 15
	// samples ≈ 0.3 ms at 48 kHz — well below perceptible.
	class FOversampler2x
	{
	public:
		static constexpr uint32_t NumTaps = 31;
		static_assert((NumTaps & 1) == 1, "Half-band FIR must have odd tap count");
		static constexpr uint32_t HalfTaps = (NumTaps - 1) / 2;

		void Prepare(double /*SampleRate*/)
		{
			// Hamming-windowed sinc at normalised cutoff 0.25 (= fs/4 at the
			// 2x rate, = fs/2 at the host rate). Symmetric around the centre
			// tap; centre value is 0.5 (the half-band cutoff convention).
			const double Pi = 3.141592653589793;
			double Sum = 0.0;
			for (uint32_t I = 0; I < NumTaps; ++I)
			{
				const int N = static_cast<int>(I) - static_cast<int>(HalfTaps);
				double H;
				if (N == 0)
				{
					H = 0.5;
				}
				else
				{
					const double Sinc = std::sin(Pi * N * 0.5) / (Pi * N);
					const double Window = 0.54 - 0.46 * std::cos(2.0 * Pi * I / (NumTaps - 1));
					H = Sinc * Window;
				}
				Coeffs[I] = static_cast<float>(H);
				Sum += H;
			}
			// Normalise so DC gain through the full up-down round trip is 1.0.
			// Up path inserts zeros (halving energy) then filters and scales by
			// 2 to compensate; down path filters and decimates. The shared
			// FIR with these coeffs sums to 1.0 by construction.
			const float Scale = static_cast<float>(1.0 / Sum);
			for (uint32_t I = 0; I < NumTaps; ++I) { Coeffs[I] *= Scale; }
			Reset();
		}

		void Reset()
		{
			UpHistory.fill(0.0f);
			DownHistory.fill(0.0f);
			DownPhase = 0;
		}

		// Process N host-rate input samples through 2x oversampling and back.
		// `WorkBuffer` (length 2*N) is built from In, passed to ProcessAt2x
		// in place, then decimated to Out. N must be <= MaxBlock.
		template<typename Inner>
		void Process(const float* In, float* Out, uint32_t N, Inner&& ProcessAt2x)
		{
			if (N == 0) { return; }

			// --- Upsample -----------------------------------------------------
			// Zero-stuff: insert one zero after each input sample. The filter
			// then runs at the 2x rate, smoothing the spectral images created
			// by zero-stuffing. For energy preservation, multiply the result
			// by 2 (compensates for the half-rate energy of the zero stream).
			for (uint32_t I = 0; I < N; ++I)
			{
				// Slide the input through the history (newest at index 0).
				for (uint32_t J = NumTaps - 1; J > 0; --J)
				{
					UpHistory[J] = UpHistory[J - 1];
				}
				UpHistory[0] = In[I];

				// Two output samples per input. Convolve against even-index
				// taps for the first, odd-index taps for the second.
				float Even = 0.0f, Odd = 0.0f;
				for (uint32_t J = 0; J < NumTaps; ++J)
				{
					if ((J & 1) == 0) { Even += Coeffs[J] * UpHistory[J]; }
					else              { Odd  += Coeffs[J] * UpHistory[J]; }
				}
				WorkBuffer[2 * I + 0] = 2.0f * Even;
				WorkBuffer[2 * I + 1] = 2.0f * Odd;
			}

			// --- Inner DSP at 2x rate ----------------------------------------
			ProcessAt2x(WorkBuffer.data(), 2 * N);

			// --- Downsample --------------------------------------------------
			// Filter the 2x stream with the same FIR, then decimate by 2 by
			// taking every other output sample (we only emit one host-rate
			// sample per two 2x-rate samples). Maintain history across blocks.
			for (uint32_t I = 0; I < N; ++I)
			{
				// Shift two new samples into the down-history.
				for (uint32_t Step = 0; Step < 2; ++Step)
				{
					for (uint32_t J = NumTaps - 1; J > 0; --J)
					{
						DownHistory[J] = DownHistory[J - 1];
					}
					DownHistory[0] = WorkBuffer[2 * I + Step];
				}
				// Convolve and emit one decimated sample per input pair.
				float Acc = 0.0f;
				for (uint32_t J = 0; J < NumTaps; ++J)
				{
					Acc += Coeffs[J] * DownHistory[J];
				}
				Out[I] = Acc;
			}
		}

		static constexpr uint32_t MaxBlock = BlockSize;

	private:
		std::array<float, NumTaps> Coeffs{};
		std::array<float, NumTaps> UpHistory{};
		std::array<float, NumTaps> DownHistory{};
		std::array<float, MaxBlock * 2> WorkBuffer{};
		uint32_t DownPhase = 0;  // currently unused; kept for API symmetry
	};
}
