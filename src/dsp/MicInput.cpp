#include "dsp/MicInput.h"

#include <cstdint>
#include <vector>

#include <miniaudio.h>

namespace NodeSynth
{
	// All miniaudio state for one capture node. Hidden here so the header stays
	// free of <miniaudio.h>.
	struct FMicInput::FMicDevice
	{
		ma_context Context{};
		bool bContextInited = false;
		ma_device Device{};
		bool bDeviceInited = false;
		// Parallel to CachedDeviceNames[1..] (the "Off" entry has no id).
		std::vector<ma_device_id> CaptureIds;
	};

	namespace
	{
		// miniaudio capture callback. Runs on the capture device's own thread;
		// the only thing it may do is push into the lock-free ring.
		void MicCaptureCallback(ma_device* Device, void* /*Output*/, const void* Input, ma_uint32 FrameCount)
		{
			auto* Self = static_cast<FMicInput*>(Device->pUserData);
			if (Self != nullptr)
			{
				Self->PushCapturedSamples(static_cast<const float*>(Input), FrameCount);
			}
		}
	}

	FMicInput::FMicInput()
		: Device(std::make_unique<FMicDevice>())
	{
		// No miniaudio work at construction — enumeration is lazy (RefreshDevices,
		// triggered from the UI) so merely creating the node never touches the
		// audio backend. Tests rely on this.
	}

	FMicInput::~FMicInput()
	{
		CloseDevice();
		if (Device->bContextInited)
		{
			ma_context_uninit(&Device->Context);
			Device->bContextInited = false;
		}
	}

	void FMicInput::RefreshDevices()
	{
		bEnumerated = true;
		CachedDeviceNames.clear();
		CachedDeviceNames.push_back("Off");
		Device->CaptureIds.clear();

		if (!Device->bContextInited)
		{
			if (ma_context_init(nullptr, 0, nullptr, &Device->Context) != MA_SUCCESS)
			{
				return;  // No backend — leave the list at just "Off".
			}
			Device->bContextInited = true;
		}

		ma_device_info* PlaybackInfos = nullptr;
		ma_uint32 PlaybackCount = 0;
		ma_device_info* CaptureInfos = nullptr;
		ma_uint32 CaptureCount = 0;
		if (ma_context_get_devices(&Device->Context, &PlaybackInfos, &PlaybackCount,
				&CaptureInfos, &CaptureCount) != MA_SUCCESS)
		{
			return;
		}

		for (ma_uint32 I = 0; I < CaptureCount; ++I)
		{
			CachedDeviceNames.push_back(CaptureInfos[I].name);
			Device->CaptureIds.push_back(CaptureInfos[I].id);
		}
	}

	void FMicInput::SetDevice(int32_t Index)
	{
		if (Index < 0) { Index = 0; }
		DeviceIdx.store(Index, std::memory_order_relaxed);
		OpenDevice(Index);
	}

	void FMicInput::OpenDevice(int32_t Index)
	{
		CloseDevice();
		if (Index <= 0)
		{
			return;  // "Off" — no capture device.
		}
		if (!Device->bContextInited)
		{
			return;  // Shouldn't happen: a non-Off index implies a prior enumerate.
		}
		const size_t ListIndex = static_cast<size_t>(Index - 1);
		if (ListIndex >= Device->CaptureIds.size())
		{
			return;
		}

		ma_device_config Config = ma_device_config_init(ma_device_type_capture);
		Config.capture.pDeviceID = &Device->CaptureIds[ListIndex];
		Config.capture.format = ma_format_f32;
		Config.capture.channels = 1;
		Config.sampleRate = static_cast<ma_uint32>(SampleRate);
		Config.dataCallback = &MicCaptureCallback;
		Config.pUserData = this;

		if (ma_device_init(&Device->Context, &Config, &Device->Device) != MA_SUCCESS)
		{
			return;  // Device unavailable / permission denied → stays silent.
		}
		if (ma_device_start(&Device->Device) != MA_SUCCESS)
		{
			ma_device_uninit(&Device->Device);
			return;
		}
		Device->bDeviceInited = true;
	}

	void FMicInput::CloseDevice()
	{
		if (Device->bDeviceInited)
		{
			// ma_device_uninit joins the capture thread — brief, UI-thread only.
			ma_device_uninit(&Device->Device);
			Device->bDeviceInited = false;
		}
	}

	bool FMicInput::IsCapturing() const
	{
		return Device && Device->bDeviceInited;
	}
}
