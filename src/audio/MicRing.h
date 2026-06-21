#pragma once

#include <atomic>
#include <cstddef>

namespace NodeSynth
{
	// Lock-free SPSC float ring for live audio capture. Producer = miniaudio's
	// capture-callback thread; consumer = the audio (Process) thread. Power-of-
	// two capacity. Both sides only touch the atomic head/tail indices and their
	// own end of the storage — no locks, no allocation, no shared mutation. Same
	// shape as TSpscMidiRing, specialised for mono float samples.
	template<size_t InCapacity>
	class TSpscAudioRing
	{
	public:
		static_assert((InCapacity & (InCapacity - 1)) == 0, "Capacity must be a power of two");
		static constexpr size_t Capacity = InCapacity;
		static constexpr size_t Mask = Capacity - 1;

		// Returns false if the ring is full (sample dropped).
		bool Push(float Sample)
		{
			const size_t CurrentTail = Tail.load(std::memory_order_relaxed);
			const size_t CurrentHead = Head.load(std::memory_order_acquire);
			const size_t Next = (CurrentTail + 1) & Mask;
			if (Next == CurrentHead)
			{
				return false;
			}
			Storage[CurrentTail] = Sample;
			Tail.store(Next, std::memory_order_release);
			return true;
		}

		// Returns false (and leaves OutSample untouched) if the ring is empty.
		bool Pop(float& OutSample)
		{
			const size_t CurrentHead = Head.load(std::memory_order_relaxed);
			const size_t CurrentTail = Tail.load(std::memory_order_acquire);
			if (CurrentHead == CurrentTail)
			{
				return false;
			}
			OutSample = Storage[CurrentHead];
			Head.store((CurrentHead + 1) & Mask, std::memory_order_release);
			return true;
		}

		// Number of samples currently available to read.
		size_t Available() const
		{
			const size_t CurrentHead = Head.load(std::memory_order_acquire);
			const size_t CurrentTail = Tail.load(std::memory_order_acquire);
			return (CurrentTail - CurrentHead) & Mask;
		}

		bool IsEmpty() const
		{
			return Head.load(std::memory_order_acquire) == Tail.load(std::memory_order_acquire);
		}

	private:
		float Storage[Capacity]{};
		std::atomic<size_t> Head{ 0 };
		std::atomic<size_t> Tail{ 0 };
	};

	// ~85 ms of slack at 48 kHz — enough to absorb capture/playback clock drift
	// and block-size mismatch between the two device callbacks.
	using FMicRing = TSpscAudioRing<4096>;
}
