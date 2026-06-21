#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio/MicRing.h"
#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	// Live microphone / line capture source. Owns its own miniaudio capture
	// device (the app's playback device in main.cpp is untouched). The capture
	// callback runs on miniaudio's own thread and pushes mono samples into a
	// lock-free SPSC ring; Process drains that ring at block start — the same
	// producer/consumer hand-off the MIDI subsystem uses.
	//
	// All miniaudio state lives behind an opaque FMicDevice (defined in the
	// .cpp) so this header — and everything that includes it (the registry,
	// the editor, the tests) — never pulls in <miniaudio.h>.
	//
	// Threading: SetParamValue stays RT-safe (atomic stores only) because the
	// command queue dispatches it on the audio thread. Device open/close is a
	// separate UI-thread-only path (SetDevice), driven by the custom property-
	// panel UI. The device persists across graph recompiles (mono nodes are
	// reused, not re-created) so Prepare never touches it.
	class FMicInput : public TNodeBase<0, 1>
	{
	public:
		FMicInput();
		~FMicInput() override;  // defined in .cpp (unique_ptr to incomplete type)

		enum EParam : uint32_t
		{
			Param_Device,
			Param_GainDb,
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "MicInput"; }

		std::vector<FPortInfo> GetInputPorts() const override { return {}; }

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Audio,
				"Live mono capture. Wire into a Vocoder's Modulator. Use\n"
				"headphones — monitoring through speakers feeds back into the mic." } };
		}

		bool IsOutputStereo(uint32_t Index) const override { (void)Index; return false; }

		std::vector<FParamInfo> GetParamInfos() const override
		{
			std::vector<FParamInfo> Infos;

			FParamInfo DeviceInfo;
			DeviceInfo.Name = "Device";
			DeviceInfo.Kind = EParamKind::Choice;
			DeviceInfo.MinValue = 0.0f;
			DeviceInfo.MaxValue = (CachedDeviceNames.size() > 1)
				? static_cast<float>(CachedDeviceNames.size() - 1) : 0.0f;
			DeviceInfo.DefaultValue = 0.0f;
			DeviceInfo.Choices = CachedDeviceNames;
			DeviceInfo.Description = "Capture device. 'Off' = no input.";
			// Rendered by the custom MicInput UI (opening a device is a UI-thread
			// action, so it can't ride the generic combo's SetParam queue path).
			DeviceInfo.bHidden = true;
			Infos.push_back(std::move(DeviceInfo));

			Infos.push_back({ "Gain", -24.0f, 24.0f, 0.0f, false, EParamKind::Float, {},
				"Input level trim in dB, applied after capture." });
			return Infos;
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_Device:  return static_cast<float>(DeviceIdx.load(std::memory_order_relaxed));
				case Param_GainDb:  return GainDb.load(std::memory_order_relaxed);
				default: return 0.0f;
			}
		}

		// RT-safe: stores only. Device open/close is the UI-thread SetDevice path.
		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_Device:
				{
					int32_t V = static_cast<int32_t>(Value);
					if (V < 0) { V = 0; }
					DeviceIdx.store(V, std::memory_order_relaxed);
					break;
				}
				case Param_GainDb:  GainDb.store(Clamp(Value, -24.0f, 24.0f), std::memory_order_relaxed); break;
				default: break;
			}
		}

		void Prepare(double InSampleRate) override
		{
			// Device + ring are deliberately untouched — the device persists
			// across recompiles and the ring is fed concurrently by the capture
			// thread. Only reset the per-Process smoother.
			SampleRate = static_cast<float>(InSampleRate);
			GainSmoother.Prepare(InSampleRate, 20.0f);
			GainSmoother.Reset(DbToLin(GainDb.load(std::memory_order_relaxed)));
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Out = GetOutputBuffer(0, 0);
			GainSmoother.SetTarget(DbToLin(GainDb.load(std::memory_order_relaxed)));
			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				float Sample = 0.0f;
				Ring.Pop(Sample);  // underrun → 0 (Sample left at its init value)
				Out[I] = Sample * GainSmoother.Tick();
			}
		}

		// A live capture device can't be sensibly cloned per voice; like the
		// MIDI input / virtual keyboard, it's a singleton-ish source.
		std::shared_ptr<INode> Clone() const override { return nullptr; }

		// Producer side. Called by the capture callback (capture thread) and by
		// tests. Pushes mono samples into the SPSC ring; drops on overrun.
		void PushCapturedSamples(const float* Mono, uint32_t Count)
		{
			if (Mono == nullptr) { return; }
			for (uint32_t I = 0; I < Count; ++I)
			{
				if (!Ring.Push(Mono[I])) { break; }
			}
		}

		// --- UI-thread-only device control (defined in MicInput.cpp) ---------
		// Re-enumerate available capture devices into the cached name list.
		void RefreshDevices();
		// Select + open a device by index (0 = Off). Stores the index and opens.
		void SetDevice(int32_t Index);
		// True while a capture device is open and running.
		bool IsCapturing() const;
		// Has RefreshDevices run at least once? (UI uses this to lazily enumerate.)
		bool IsEnumerated() const { return bEnumerated; }
		// [0] is always "Off"; [1..] are capture device names.
		const std::vector<std::string>& DeviceNames() const { return CachedDeviceNames; }

	private:
		static float Clamp(float V, float Min, float Max)
		{
			if (V < Min) { return Min; }
			if (V > Max) { return Max; }
			return V;
		}

		static float DbToLin(float Db) { return std::pow(10.0f, Db / 20.0f); }

		void OpenDevice(int32_t Index);   // .cpp
		void CloseDevice();               // .cpp

		FMicRing Ring;
		std::atomic<int32_t> DeviceIdx{ 0 };  // 0 = Off
		std::atomic<float>   GainDb{ 0.0f };

		float SampleRate = 48000.0f;          // UI-thread only (Prepare / OpenDevice)
		FOnePoleSmoother GainSmoother;

		bool bEnumerated = false;             // UI-thread only
		std::vector<std::string> CachedDeviceNames{ "Off" };

		struct FMicDevice;
		std::unique_ptr<FMicDevice> Device;
	};
}
