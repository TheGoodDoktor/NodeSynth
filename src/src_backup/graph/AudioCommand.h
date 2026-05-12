#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "dsp/Node.h"

namespace NodeSynth
{
	// Audio-thread-bound command. Pushed by the UI thread (e.g. on slider drag,
	// or by patch deserialize replay), drained by the audio thread at block start.
	//
	// The variant is intentionally narrow for this milestone: only SetParam.
	// NoteOn / NoteOff land alongside polyphony; structural commands (AddNode /
	// RemoveNode / Connect / Disconnect) remain on the snapshot-swap path
	// because RT-safe structural mutation is a much larger engineering lift.
	enum class EAudioCommand : uint8_t
	{
		SetParam,
		NoteOn,    // ParamIndex = MIDI note 0..127, Value = velocity 0..1
		NoteOff,   // ParamIndex = MIDI note 0..127, Value ignored
	};

	struct FAudioCommand
	{
		EAudioCommand Type = EAudioCommand::SetParam;
		FNodeId NodeId = 0;        // ignored for NoteOn / NoteOff (broadcast)
		uint32_t ParamIndex = 0;   // for note events: the MIDI note number
		float Value = 0.0f;        // for SetParam: the param value; for NoteOn: velocity 0..1

		static FAudioCommand MakeSetParam(FNodeId InNodeId, uint32_t InParamIndex, float InValue)
		{
			FAudioCommand Cmd;
			Cmd.Type = EAudioCommand::SetParam;
			Cmd.NodeId = InNodeId;
			Cmd.ParamIndex = InParamIndex;
			Cmd.Value = InValue;
			return Cmd;
		}

		static FAudioCommand MakeNoteOn(uint8_t Note, float Velocity)
		{
			FAudioCommand Cmd;
			Cmd.Type = EAudioCommand::NoteOn;
			Cmd.NodeId = 0;
			Cmd.ParamIndex = Note;
			Cmd.Value = Velocity;
			return Cmd;
		}

		static FAudioCommand MakeNoteOff(uint8_t Note)
		{
			FAudioCommand Cmd;
			Cmd.Type = EAudioCommand::NoteOff;
			Cmd.NodeId = 0;
			Cmd.ParamIndex = Note;
			Cmd.Value = 0.0f;
			return Cmd;
		}
	};

	// Lock-free SPSC ring, power-of-two capacity. Writer = UI thread; reader =
	// audio thread. Same shape as FMidiRing — both sides only touch atomic
	// indices and their own half of the storage.
	template<size_t InCapacity>
	class TSpscAudioCommandRing
	{
	public:
		static_assert((InCapacity & (InCapacity - 1)) == 0, "Capacity must be a power of two");
		static constexpr size_t Capacity = InCapacity;
		static constexpr size_t Mask = Capacity - 1;

		bool Push(const FAudioCommand& Command)
		{
			const size_t CurrentTail = Tail.load(std::memory_order_relaxed);
			const size_t CurrentHead = Head.load(std::memory_order_acquire);
			const size_t Next = (CurrentTail + 1) & Mask;
			if (Next == CurrentHead)
			{
				return false;
			}
			Storage[CurrentTail] = Command;
			Tail.store(Next, std::memory_order_release);
			return true;
		}

		bool Pop(FAudioCommand& OutCommand)
		{
			const size_t CurrentHead = Head.load(std::memory_order_relaxed);
			const size_t CurrentTail = Tail.load(std::memory_order_acquire);
			if (CurrentHead == CurrentTail)
			{
				return false;
			}
			OutCommand = Storage[CurrentHead];
			Head.store((CurrentHead + 1) & Mask, std::memory_order_release);
			return true;
		}

		bool IsEmpty() const
		{
			return Head.load(std::memory_order_acquire) == Tail.load(std::memory_order_acquire);
		}

	private:
		FAudioCommand Storage[Capacity]{};
		std::atomic<size_t> Head{ 0 };
		std::atomic<size_t> Tail{ 0 };
	};

	using FAudioCommandRing = TSpscAudioCommandRing<512>;

	// UI-side helper: bundles a ring pointer and a target node id so call sites
	// can push commands without juggling both. A null ring is a no-op (handy for
	// tests and code paths that haven't been wired up yet).
	struct FCommandSink
	{
		FAudioCommandRing* Ring = nullptr;
		FNodeId NodeId = 0;

		void SetParam(uint32_t ParamIndex, float Value) const
		{
			if (Ring != nullptr)
			{
				Ring->Push(FAudioCommand::MakeSetParam(NodeId, ParamIndex, Value));
			}
		}

		// Note events broadcast to every voice allocator in the snapshot — the
		// NodeId on this sink is ignored.
		void NoteOn(uint8_t Note, float Velocity) const
		{
			if (Ring != nullptr)
			{
				Ring->Push(FAudioCommand::MakeNoteOn(Note, Velocity));
			}
		}

		void NoteOff(uint8_t Note) const
		{
			if (Ring != nullptr)
			{
				Ring->Push(FAudioCommand::MakeNoteOff(Note));
			}
		}
	};
}
