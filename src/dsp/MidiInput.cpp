#include "dsp/MidiInput.h"

#include <RtMidi.h>

#include <cmath>
#include <cstdio>

namespace NodeSynth
{
	namespace
	{
		// A440 = MIDI note 69. Returns frequency in Hz for an integer MIDI note.
		float NoteToFrequency(uint8_t Note)
		{
			return 440.0f * std::pow(2.0f, (static_cast<float>(Note) - 69.0f) / 12.0f);
		}

		void RtMidiCallback(double /*Timestamp*/, std::vector<unsigned char>* Message, void* UserData)
		{
			if (Message == nullptr || UserData == nullptr)
			{
				return;
			}
			auto* Node = static_cast<FMidiInput*>(UserData);
			Node->OnMidiMessage(Message->data(), Message->size());
		}
	}

	FMidiInput::FMidiInput()
	{
		try
		{
			Rt = std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, "NodeSynth");
			Rt->setCallback(&RtMidiCallback, this);
			Rt->ignoreTypes(true, true, true);  // ignore SysEx, timing, active sensing
		}
		catch (const std::exception& E)
		{
			std::fprintf(stderr, "RtMidi init failed: %s\n", E.what());
			Rt.reset();
		}
	}

	FMidiInput::~FMidiInput()
	{
		if (Rt && Rt->isPortOpen())
		{
			Rt->closePort();
		}
		// Rt unique_ptr destructor joins RtMidi's callback thread here.
	}

	std::vector<FParamInfo> FMidiInput::GetParamInfos() const
	{
		FParamInfo Dev;
		Dev.Name = "Device";
		Dev.Kind = EParamKind::Choice;
		Dev.Choices.push_back("(none)");
		for (const auto& Name : DeviceNames)
		{
			Dev.Choices.push_back(Name);
		}
		Dev.MinValue = 0.0f;
		Dev.MaxValue = static_cast<float>(Dev.Choices.size() - 1);
		Dev.DefaultValue = 0.0f;

		FParamInfo Ch;
		Ch.Name = "Channel";
		Ch.Kind = EParamKind::Choice;
		Ch.Choices = { "Omni", "1", "2", "3", "4", "5", "6", "7", "8",
			"9", "10", "11", "12", "13", "14", "15", "16" };
		Ch.MinValue = 0.0f;
		Ch.MaxValue = 16.0f;
		Ch.DefaultValue = 0.0f;

		return { Dev, Ch };
	}

	float FMidiInput::GetParamValue(uint32_t Index) const
	{
		switch (Index)
		{
			case Param_Device:        return static_cast<float>(RequestedPort.load() + 1);
			case Param_ChannelFilter: return static_cast<float>(ChannelFilter.load());
			default: return 0.0f;
		}
	}

	void FMidiInput::SetParamValue(uint32_t Index, float Value)
	{
		switch (Index)
		{
			case Param_Device:
			{
				// The combo includes "(none)" as index 0; map to -1.
				const int32_t V = static_cast<int32_t>(Value) - 1;
				RequestedPort.store(V);

				// UI thread drives device open/close. Refresh the list and reopen.
				const_cast<FMidiInput*>(this)->RefreshDeviceList();
				const_cast<FMidiInput*>(this)->ReopenIfNeeded();
				break;
			}
			case Param_ChannelFilter:
			{
				int32_t V = static_cast<int32_t>(Value);
				if (V < 0) { V = 0; }
				if (V > 16) { V = 16; }
				ChannelFilter.store(V);
				break;
			}
			default: break;
		}
	}

	void FMidiInput::RefreshDeviceList()
	{
		DeviceNames.clear();
		if (!Rt)
		{
			return;
		}
		try
		{
			const uint32_t Count = Rt->getPortCount();
			for (uint32_t I = 0; I < Count; ++I)
			{
				DeviceNames.push_back(Rt->getPortName(I));
			}
		}
		catch (const std::exception& E)
		{
			std::fprintf(stderr, "RtMidi list failed: %s\n", E.what());
		}
	}

	void FMidiInput::ReopenIfNeeded()
	{
		const int32_t Want = RequestedPort.load();
		if (Want == OpenedPort)
		{
			return;
		}
		if (!Rt)
		{
			return;
		}
		try
		{
			if (Rt->isPortOpen())
			{
				Rt->closePort();
			}
			if (Want >= 0 && Want < static_cast<int32_t>(Rt->getPortCount()))
			{
				Rt->openPort(static_cast<uint32_t>(Want), "NodeSynth In");
			}
			OpenedPort = Want;
		}
		catch (const std::exception& E)
		{
			std::fprintf(stderr, "RtMidi openPort failed: %s\n", E.what());
			OpenedPort = -1;
		}
	}

	void FMidiInput::OnMidiMessage(const unsigned char* Bytes, size_t Length)
	{
		// Only handle three-byte channel-voice messages for Phase 2.
		if (Length < 3)
		{
			return;
		}
		FMidiEvent Event;
		Event.Status = Bytes[0];
		Event.Data1 = Bytes[1];
		Event.Data2 = Bytes[2];
		Ring.Push(Event);  // dropped silently if ring is full
	}

	void FMidiInput::Prepare(double InSampleRate)
	{
		SampleRate = InSampleRate;
		NumHeldNotes = 0;
		CurrentGate = 0.0f;
		CurrentFrequency = 440.0f;
		CurrentVelocity = 0.0f;
	}

	void FMidiInput::Process(const FProcessContext& Ctx)
	{
		const int32_t Filter = ChannelFilter.load(std::memory_order_relaxed);

		// Drain all pending events at block start. Sample-accurate dispatch is a
		// Phase 3+ concern; for now all events apply at sample 0 of this block.
		FMidiEvent Event;
		while (Ring.Pop(Event))
		{
			const uint8_t Type = Event.Status & 0xF0;
			const uint8_t Channel = (Event.Status & 0x0F) + 1;  // 1..16
			if (Filter != 0 && static_cast<int32_t>(Channel) != Filter)
			{
				continue;
			}

			// Note-on with velocity 0 is equivalent to note-off (running-status convention).
			const bool bNoteOn = (Type == 0x90) && (Event.Data2 > 0);
			const bool bNoteOff = (Type == 0x80) || ((Type == 0x90) && (Event.Data2 == 0));

			if (bNoteOn)
			{
				if (NumHeldNotes < MaxHeldNotes)
				{
					HeldNotes[NumHeldNotes] = Event.Data1;
					HeldVelocities[NumHeldNotes] = Event.Data2;
					++NumHeldNotes;
				}
				else
				{
					// Drop the oldest; shift. Rare path (>16 simultaneous notes).
					for (size_t I = 1; I < MaxHeldNotes; ++I)
					{
						HeldNotes[I - 1] = HeldNotes[I];
						HeldVelocities[I - 1] = HeldVelocities[I];
					}
					HeldNotes[MaxHeldNotes - 1] = Event.Data1;
					HeldVelocities[MaxHeldNotes - 1] = Event.Data2;
				}
				CurrentFrequency = NoteToFrequency(Event.Data1);
				CurrentVelocity = static_cast<float>(Event.Data2) / 127.0f;
				CurrentGate = 1.0f;
			}
			else if (bNoteOff)
			{
				// Remove the released note from the stack, preserving order of the rest.
				for (size_t I = 0; I < NumHeldNotes; ++I)
				{
					if (HeldNotes[I] == Event.Data1)
					{
						for (size_t J = I + 1; J < NumHeldNotes; ++J)
						{
							HeldNotes[J - 1] = HeldNotes[J];
							HeldVelocities[J - 1] = HeldVelocities[J];
						}
						--NumHeldNotes;
						break;
					}
				}
				if (NumHeldNotes == 0)
				{
					CurrentGate = 0.0f;
				}
				else
				{
					const uint8_t Top = HeldNotes[NumHeldNotes - 1];
					CurrentFrequency = NoteToFrequency(Top);
					CurrentVelocity = static_cast<float>(HeldVelocities[NumHeldNotes - 1]) / 127.0f;
					// Gate stays high — legato across held notes.
				}
			}
		}

		float* Gate = GetOutputBuffer(Output_Gate);
		float* Freq = GetOutputBuffer(Output_Frequency);
		float* Vel = GetOutputBuffer(Output_Velocity);
		for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
		{
			Gate[I] = CurrentGate;
			Freq[I] = CurrentFrequency;
			Vel[I] = CurrentVelocity;
		}
	}
}
