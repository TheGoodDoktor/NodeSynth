#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "midi/MidiRing.h"

class RtMidiIn;

namespace NodeSynth
{
	class FVoiceAllocator;
	class FMidiCC;
	struct FProcessContext;

	// Project-level MIDI input subsystem. Owns one RtMidiIn instance and two
	// SPSC rings (note events for the audio thread, CC events for the UI
	// thread / MIDI Learn). Replaces the FMidiInput node — note input is a
	// transport concern, not a graph concern, so it lives outside the graph.
	//
	// One instance is held by FAudioState (in main.cpp). FGraphModel::Compile
	// pushes the current snapshot's voice allocator list via SetVoiceAllocators
	// so every block can dispatch notes directly to the audio-thread state.
	// The audio callback calls Process(Ctx) once per block before
	// FAudioGraph::Process, ensuring notes captured this block reach the
	// allocators before they emit per-voice gates / frequencies.
	//
	// Threading mirrors the old FMidiInput: device open/close on the UI
	// thread, RtMidi callback (its own thread) writes to both rings, audio
	// thread drains the note ring + dispatches to allocators, UI thread
	// drains the CC ring (for MIDI Learn).
	class FMidiDeviceManager
	{
	public:
		FMidiDeviceManager();
		~FMidiDeviceManager();

		FMidiDeviceManager(const FMidiDeviceManager&) = delete;
		FMidiDeviceManager& operator=(const FMidiDeviceManager&) = delete;

		// UI-thread API ---------------------------------------------------------

		// Refresh the device list from RtMidi (cheap; OS-cached). Returns the
		// snapshot. Call before showing the device combo so hot-plugged
		// controllers show up.
		std::vector<std::string> GetDeviceNames() const;

		// Currently-selected device port, or -1 if no device is open.
		int32_t GetSelectedPort() const { return RequestedPort.load(std::memory_order_relaxed); }

		// Open the device at the given port index, or close all devices if -1.
		// Idempotent — re-selecting the current port does nothing.
		void SetDevicePort(int32_t Port);

		// True when RtMidi initialised successfully. False if the platform
		// MIDI subsystem is unavailable (e.g. headless test harness).
		bool IsAvailable() const { return Rt != nullptr; }

		// Channel filter: 0 = Omni, 1..16 = specific channel. Notes outside
		// the filter are ignored entirely (CCs always pass through — MIDI
		// Learn uses them regardless).
		void SetChannelFilter(int32_t Channel);
		int32_t GetChannelFilter() const { return ChannelFilter.load(std::memory_order_relaxed); }

		// Drain all currently-buffered CC events. The visitor receives
		// (channel 1..16, cc 0..127, value 0..127). Called once per UI frame
		// from FGraphEditorPanel::Draw to feed MIDI Learn.
		template<typename Visitor>
		void DrainCcEvents(Visitor&& Visit)
		{
			FMidiEvent Event;
			while (CcRing.Pop(Event))
			{
				const uint8_t Channel = (Event.Status & 0x0F) + 1;
				Visit(Channel, Event.Data1, Event.Data2);
			}
		}

		// Audio-thread API ------------------------------------------------------

		// Hand over the active snapshot's voice allocator list. Pointers are
		// valid for the lifetime of the snapshot's shared_ptr; FGraphModel::Compile
		// updates this on every recompile.
		void SetVoiceAllocators(std::vector<FVoiceAllocator*> InAllocators);

		// Hand over the active snapshot's FMidiCC node list. Updated by Compile.
		// Audio thread visits every entry when draining the audio CC ring so
		// each node can filter for its assigned (CC#, Channel).
		void SetMidiCcNodes(std::vector<FMidiCC*> InNodes);

		// Drain the note ring + audio CC ring and dispatch to every
		// registered allocator / MIDI CC node. Runs on the audio thread
		// once per block, before FAudioGraph::Process.
		void Process(const FProcessContext& Ctx);

		// Called from the RtMidi callback thread. Public so the C-style free
		// function in MidiDeviceManager.cpp can reach it. Splits incoming
		// messages by status nibble: $Bx → both CC rings, everything else
		// → note ring.
		void OnMidiMessage(const unsigned char* Bytes, size_t Length);

	private:
		void RefreshDeviceListLocked() const;

		std::unique_ptr<RtMidiIn> Rt;
		FMidiRing NoteRing;
		FMidiCcRing CcRing;        // drained on UI thread (MIDI Learn)
		FMidiCcRing AudioCcRing;   // drained on audio thread (FMidiCC nodes)

		// UI-thread state.
		mutable std::vector<std::string> DeviceNames;
		std::atomic<int32_t> RequestedPort{ -1 };
		int32_t OpenedPort = -1;
		std::atomic<int32_t> ChannelFilter{ 0 };

		// Audio-thread state.
		std::vector<FVoiceAllocator*> Allocators;
		std::vector<FMidiCC*> MidiCcNodes;
	};
}
