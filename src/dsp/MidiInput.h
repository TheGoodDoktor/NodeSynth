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
				{ "Gate",      EPortType::Control },
				{ "Frequency", EPortType::Control },
				{ "Velocity",  EPortType::Control },
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

	private:
		void RefreshDeviceList();
		void ReopenIfNeeded();

		std::unique_ptr<RtMidiIn> Rt;
		FMidiRing Ring;

		// UI-thread data. Device list is rebuilt each time we open the param panel.
		std::vector<std::string> DeviceNames;
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
	};
}
