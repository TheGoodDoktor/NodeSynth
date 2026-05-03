#include "midi/MidiDeviceManager.h"

#include <RtMidi.h>

#include <cstdio>

#include "dsp/Node.h"
#include "dsp/VoiceAllocator.h"

namespace NodeSynth
{
	namespace
	{
		void RtMidiCallback(double /*Timestamp*/, std::vector<unsigned char>* Message, void* UserData)
		{
			if (Message == nullptr || UserData == nullptr) { return; }
			auto* Mgr = static_cast<FMidiDeviceManager*>(UserData);
			Mgr->OnMidiMessage(Message->data(), Message->size());
		}
	}

	FMidiDeviceManager::FMidiDeviceManager()
	{
		try
		{
			Rt = std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, "NodeSynth");
			Rt->setCallback(&RtMidiCallback, this);
			Rt->ignoreTypes(true, true, true);  // SysEx, timing, active sensing
		}
		catch (const std::exception& E)
		{
			std::fprintf(stderr, "RtMidi init failed: %s\n", E.what());
			Rt.reset();
		}
	}

	FMidiDeviceManager::~FMidiDeviceManager()
	{
		if (Rt && Rt->isPortOpen())
		{
			Rt->closePort();
		}
		// RtMidiIn's destructor joins its callback thread.
	}

	std::vector<std::string> FMidiDeviceManager::GetDeviceNames() const
	{
		RefreshDeviceListLocked();
		return DeviceNames;
	}

	void FMidiDeviceManager::RefreshDeviceListLocked() const
	{
		DeviceNames.clear();
		if (!Rt) { return; }
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

	void FMidiDeviceManager::SetDevicePort(int32_t Port)
	{
		RequestedPort.store(Port, std::memory_order_relaxed);
		if (!Rt) { return; }
		if (Port == OpenedPort) { return; }
		try
		{
			if (Rt->isPortOpen())
			{
				Rt->closePort();
			}
			RefreshDeviceListLocked();
			if (Port >= 0 && Port < static_cast<int32_t>(Rt->getPortCount()))
			{
				Rt->openPort(static_cast<uint32_t>(Port), "NodeSynth In");
			}
			OpenedPort = Port;
		}
		catch (const std::exception& E)
		{
			std::fprintf(stderr, "RtMidi openPort failed: %s\n", E.what());
			OpenedPort = -1;
		}
	}

	void FMidiDeviceManager::SetChannelFilter(int32_t Channel)
	{
		if (Channel < 0) { Channel = 0; }
		if (Channel > 16) { Channel = 16; }
		ChannelFilter.store(Channel, std::memory_order_relaxed);
	}

	void FMidiDeviceManager::SetVoiceAllocators(std::vector<FVoiceAllocator*> InAllocators)
	{
		Allocators = std::move(InAllocators);
	}

	void FMidiDeviceManager::OnMidiMessage(const unsigned char* Bytes, size_t Length)
	{
		if (Length < 3) { return; }
		FMidiEvent Event;
		Event.Status = Bytes[0];
		Event.Data1 = Bytes[1];
		Event.Data2 = Bytes[2];
		// Route CC ($Bx) to the UI-thread ring (MIDI Learn drains it). All
		// other channel-voice messages go to the note ring (audio thread).
		if ((Event.Status & 0xF0) == 0xB0)
		{
			CcRing.Push(Event);
		}
		else
		{
			NoteRing.Push(Event);
		}
	}

	void FMidiDeviceManager::Process(const FProcessContext& /*Ctx*/)
	{
		const int32_t Filter = ChannelFilter.load(std::memory_order_relaxed);
		FMidiEvent Event;
		while (NoteRing.Pop(Event))
		{
			const uint8_t Type = Event.Status & 0xF0;
			const uint8_t Channel = (Event.Status & 0x0F) + 1;  // 1..16
			if (Filter != 0 && static_cast<int32_t>(Channel) != Filter)
			{
				continue;
			}
			const bool bNoteOn = (Type == 0x90) && (Event.Data2 > 0);
			const bool bNoteOff = (Type == 0x80) || ((Type == 0x90) && (Event.Data2 == 0));
			if (bNoteOn)
			{
				const float Vel = static_cast<float>(Event.Data2) / 127.0f;
				for (FVoiceAllocator* Alloc : Allocators)
				{
					Alloc->HandleNoteOn(Event.Data1, Vel);
				}
			}
			else if (bNoteOff)
			{
				for (FVoiceAllocator* Alloc : Allocators)
				{
					Alloc->HandleNoteOff(Event.Data1);
				}
			}
		}
	}
}
