#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace NodeSynth
{
	struct FMidiEvent
	{
		uint8_t Status = 0;  // bits 7..4: type (0x9 note-on, 0x8 note-off, 0xB CC, 0xE pitch-bend); bits 3..0: channel
		uint8_t Data1 = 0;
		uint8_t Data2 = 0;
	};

	// Lock-free SPSC ring, power-of-two capacity. Writer = RtMidi callback thread;
	// reader = audio thread. Both threads only access atomic indices + their own
	// side of the storage, so no locking, no allocation, no shared mutation.
	template<size_t InCapacity>
	class TSpscMidiRing
	{
	public:
		static_assert((InCapacity & (InCapacity - 1)) == 0, "Capacity must be a power of two");
		static constexpr size_t Capacity = InCapacity;
		static constexpr size_t Mask = Capacity - 1;

		// Returns false if the ring is full (event dropped).
		bool Push(const FMidiEvent& Event)
		{
			const size_t CurrentTail = Tail.load(std::memory_order_relaxed);
			const size_t CurrentHead = Head.load(std::memory_order_acquire);
			const size_t Next = (CurrentTail + 1) & Mask;
			if (Next == CurrentHead)
			{
				return false;
			}
			Storage[CurrentTail] = Event;
			Tail.store(Next, std::memory_order_release);
			return true;
		}

		bool Pop(FMidiEvent& OutEvent)
		{
			const size_t CurrentHead = Head.load(std::memory_order_relaxed);
			const size_t CurrentTail = Tail.load(std::memory_order_acquire);
			if (CurrentHead == CurrentTail)
			{
				return false;
			}
			OutEvent = Storage[CurrentHead];
			Head.store((CurrentHead + 1) & Mask, std::memory_order_release);
			return true;
		}

		bool IsEmpty() const
		{
			return Head.load(std::memory_order_acquire) == Tail.load(std::memory_order_acquire);
		}

	private:
		FMidiEvent Storage[Capacity]{};
		std::atomic<size_t> Head{ 0 };
		std::atomic<size_t> Tail{ 0 };
	};

	using FMidiRing = TSpscMidiRing<256>;

	// Same SPSC ring shape, separate instance for CC events drained by the UI
	// thread. Note events go to FMidiRing (audio-thread consumer); CC events
	// go to FMidiCcRing (UI-thread consumer for MIDI Learn / mapping
	// dispatch). Two rings keeps each path strictly single-producer /
	// single-consumer.
	using FMidiCcRing = TSpscMidiRing<256>;
}
