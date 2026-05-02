#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "dsp/Node.h"
#include "midi/MidiRing.h"

class RtMidiIn;

namespace NodeSynth
{
	// Monophonic last-note-wins MIDI input. Three Control outputs: Gate, Frequency (Hz), Velocity (0..1).
	// The RtMidi instance is owned by this node and its callback thread pushes events into an
	// internal SPSC ring; Process() drains the ring at block start on the audio thread.
	//
	// Lifetime note: RtMidiIn's destructor joins its callback thread, so this node's destructor
	// is not strictly RT-safe. In practice, graph edits drop the old snapshot's shared_ptr on
	// the UI thread during the next compile — see CLAUDE.md.
	class FMidiInput : public TNodeBase<0, 3>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Device,        // Choice: index into device list
			Param_ChannelFilter, // 0 = Omni, 1..16 = specific MIDI channel
			Param_COUNT,
		};

		enum EOutput : uint32_t
		{
			Output_Gate,
			Output_Frequency,
			Output_Velocity,
		};

		FMidiInput();
		~FMidiInput() override;

		FMidiInput(const FMidiInput&) = delete;
		FMidiInput& operator=(const FMidiInput&) = delete;

		const char* GetTypeName() const override { return "MIDI"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			return {};
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return
			{
				{ "Gate",      EPortType::Control,
					"1.0 while a note is held, 0.0 otherwise." },
				{ "Frequency", EPortType::Control,
					"Most recent note's frequency in Hz." },
				{ "Velocity",  EPortType::Control,
					"Most recent note's velocity (0..1)." },
			};
		}

		std::vector<FParamInfo> GetParamInfos() const override;
		float GetParamValue(uint32_t Index) const override;
		void SetParamValue(uint32_t Index, float Value) override;

		// Owns an RtMidiIn instance + a callback thread. Cloning would attempt
		// to open the same MIDI device twice. Non-cloneable.
		std::shared_ptr<INode> Clone() const override { return nullptr; }

		void Prepare(double SampleRate) override;
		void Process(const FProcessContext& Ctx) override;

		// Called from the RtMidi callback thread. Public so the C-style
		// free-function callback in MidiInput.cpp can reach it.
		void OnMidiMessage(const unsigned char* Bytes, size_t Length);

		// Set by FGraphModel::Compile so this input can dispatch real-MIDI
		// note events to the snapshot's voice allocators directly on the audio
		// thread (no ring round-trip). Pointers are valid for the lifetime of
		// the compiled snapshot.
		void SetVoiceAllocators(std::vector<class FVoiceAllocator*> InAllocators)
		{
			Allocators = std::move(InAllocators);
		}

		// UI-thread API for MIDI Learn: drain all currently-buffered CC
		// events. The visitor receives (channel 1..16, cc 0..127, value 0..127)
		// per event and runs once per drained message. Caller invokes once
		// per UI frame from FGraphEditorPanel::Draw.
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

		// Live status snapshot used by the property-panel diagnostic block.
		// Returns the number of RtMidi-visible devices and the index of the
		// currently-opened port (or -1 if none).
		struct FStatus
		{
			bool bRtMidiAvailable = false;
			std::vector<std::string> Devices;  // copy of the device list
			int32_t OpenedPort = -1;           // -1 if no device open
		};
		FStatus GetStatus() const;

	private:
		void RefreshDeviceList() const;
		void ReopenIfNeeded();

		std::unique_ptr<RtMidiIn> Rt;
		FMidiRing Ring;        // note events → audio thread
		FMidiCcRing CcRing;    // CC events  → UI thread (MIDI Learn)

		// UI-thread data. Refreshed every time GetParamInfos runs (per frame
		// while the property panel is showing) so the Device combo stays
		// current with hot-plugged controllers. Mutable because GetParamInfos
		// is const and the standard property-panel widget machinery wants it
		// that way.
		mutable std::vector<std::string> DeviceNames;
		std::atomic<int32_t> RequestedPort{ -1 };  // -1 = closed
		int32_t OpenedPort = -1;

		std::atomic<int32_t> ChannelFilter{ 0 };  // 0 = Omni, 1..16

		// Audio-thread allocator state. Fixed-capacity stack of held note numbers.
		static constexpr size_t MaxHeldNotes = 16;
		uint8_t HeldNotes[MaxHeldNotes] = {};
		uint8_t HeldVelocities[MaxHeldNotes] = {};
		size_t NumHeldNotes = 0;

		float CurrentGate = 0.0f;
		float CurrentFrequency = 440.0f;
		float CurrentVelocity = 0.0f;
		double SampleRate = 48000.0;

		// Allocator dispatch list (audio-thread only; populated by Compile).
		std::vector<class FVoiceAllocator*> Allocators;
	};
}
