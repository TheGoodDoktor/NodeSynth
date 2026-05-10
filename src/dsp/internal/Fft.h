#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace NodeSynth
{
	namespace Internal
	{
		// In-place radix-2 iterative Cooley-Tukey FFT. N must be a power
		// of two. Direction = +1 forward, -1 inverse. The inverse leaves
		// the result un-scaled — caller divides by N to recover the input.
		//
		// Used by the wavetable mip generator. Not optimised; runs once
		// per frame at load time, never on the audio thread. ~50 lines is
		// enough; pulling in KissFFT or PocketFFT would be overkill.
		inline void Fft(std::complex<float>* X, std::size_t N, int Direction)
		{
			// Bit-reversal permutation. For each I, find the bit-reversed
			// index J and swap X[I] with X[J] when I < J.
			std::size_t J = 0;
			for (std::size_t I = 1; I < N; ++I)
			{
				std::size_t Bit = N >> 1;
				for (; (J & Bit) != 0; Bit >>= 1)
				{
					J ^= Bit;
				}
				J ^= Bit;
				if (I < J)
				{
					std::swap(X[I], X[J]);
				}
			}

			// Cooley-Tukey butterfly. Doubles the segment length each pass.
			for (std::size_t Len = 2; Len <= N; Len <<= 1)
			{
				const float Theta = static_cast<float>(Direction)
					* 2.0f * 3.14159265358979323846f
					/ static_cast<float>(Len);
				const std::complex<float> WLen(std::cos(Theta), std::sin(Theta));
				const std::size_t Half = Len >> 1;
				for (std::size_t Block = 0; Block < N; Block += Len)
				{
					std::complex<float> W(1.0f, 0.0f);
					for (std::size_t K = 0; K < Half; ++K)
					{
						const std::complex<float> U = X[Block + K];
						const std::complex<float> V = X[Block + K + Half] * W;
						X[Block + K] = U + V;
						X[Block + K + Half] = U - V;
						W *= WLen;
					}
				}
			}
		}
	}
}
