#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	// MIDI Continuous Controller source. Outputs a smoothed Control value
	// in [Min, Max] driven by an assigned MIDI CC. Distinct from per-param
	// MIDI Learn (which binds a CC to one specific param via the audio
	// command queue) — this node exposes the CC as a graph-routable
	// Control source so it can be remapped, summed, fed to a mod matrix,
	// etc.
	//
	// Threading: the project-level FMidiDeviceManager owns the device and
	// drains its audio CC ring once per block, calling OnCcEvent on every
	// registered FMidiCC. The node filters by its (CC#, Channel) params
	// and latches the matching raw value. Process smooths the latched
	// value into the output buffer.
	class FMidiCC : public TNodeBase<0, 1>
	{
	public:
		enum EParam : uint32_t
		{
			Param_Cc,        // 0..127  (Float clamped to int)
			Param_Channel,   // Choice: Omni / 1..16
			Param_Min,       // output range low end
			Param_Max,       // output range high end
			Param_SmoothMs,  // one-pole smoother TC, log-scaled
			Param_COUNT,
		};

		const char* GetTypeName() const override { return "MidiCC"; }

		std::vector<FPortInfo> GetInputPorts() const override { return {}; }

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			return { { "Out", EPortType::Control,
				"Smoothed CC value scaled to [Min, Max]." } };
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			FParamInfo Cc{};
			Cc.Name = "CC";
			Cc.MinValue = 0.0f;
			Cc.MaxValue = 127.0f;
			Cc.DefaultValue = 1.0f;
			Cc.Kind = EParamKind::Float;
			Cc.Description =
				"MIDI CC number (0..127). Click Learn in the property panel\n"
				"to assign by moving a hardware controller.";

			FParamInfo Ch{};
			Ch.Name = "Channel";
			Ch.MinValue = 0.0f;
			Ch.MaxValue = 16.0f;
			Ch.DefaultValue = 0.0f;
			Ch.Kind = EParamKind::Choice;
			Ch.Choices = {
				"Omni", "1", "2", "3", "4", "5", "6", "7", "8",
				"9", "10", "11", "12", "13", "14", "15", "16",
			};
			Ch.Description = "MIDI channel filter. Omni passes any channel.";

			FParamInfo Mn{};
			Mn.Name = "Min";
			Mn.MinValue = -20000.0f;
			Mn.MaxValue = 20000.0f;
			Mn.DefaultValue = 0.0f;
			Mn.Kind = EParamKind::Float;
			Mn.Description =
				"Output value when CC reads 0. Drag or double-click to type.";
			Mn.bUseInputBox = true;

			FParamInfo Mx{};
			Mx.Name = "Max";
			Mx.MinValue = -20000.0f;
			Mx.MaxValue = 20000.0f;
			Mx.DefaultValue = 1.0f;
			Mx.Kind = EParamKind::Float;
			Mx.Description =
				"Output value when CC reads 127. Drag or double-click to type.";
			Mx.bUseInputBox = true;

			FParamInfo Sm{};
			Sm.Name = "Smooth";
			Sm.MinValue = 0.5f;
			Sm.MaxValue = 200.0f;
			Sm.DefaultValue = 5.0f;
			Sm.bLogarithmic = true;
			Sm.Kind = EParamKind::Float;
			Sm.Description =
				"One-pole smoother time constant in ms. Fast (~5 ms) feels\n"
				"responsive; slow (>50 ms) masks the 7-bit step quantisation.";

			return { Cc, Ch, Mn, Mx, Sm };
		}

		float GetParamValue(uint32_t Index) const override
		{
			switch (Index)
			{
				case Param_Cc:       return static_cast<float>(CcNumber.load(std::memory_order_relaxed));
				case Param_Channel:  return static_cast<float>(ChannelFilter.load(std::memory_order_relaxed));
				case Param_Min:      return Min.load(std::memory_order_relaxed);
				case Param_Max:      return Max.load(std::memory_order_relaxed);
				case Param_SmoothMs: return SmoothMs.load(std::memory_order_relaxed);
				default:             return 0.0f;
			}
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			switch (Index)
			{
				case Param_Cc:
				{
					int32_t V = static_cast<int32_t>(Value);
					if (V < 0)   { V = 0; }
					if (V > 127) { V = 127; }
					CcNumber.store(static_cast<uint8_t>(V), std::memory_order_relaxed);
					break;
				}
				case Param_Channel:
				{
					int32_t V = static_cast<int32_t>(Value);
					if (V < 0)  { V = 0; }
					if (V > 16) { V = 16; }
					ChannelFilter.store(static_cast<uint8_t>(V), std::memory_order_relaxed);
					break;
				}
				case Param_Min:
				{
					Min.store(Value, std::memory_order_relaxed);
					break;
				}
				case Param_Max:
				{
					Max.store(Value, std::memory_order_relaxed);
					break;
				}
				case Param_SmoothMs:
				{
					float V = Value;
					if (V < 0.5f)   { V = 0.5f; }
					if (V > 200.0f) { V = 200.0f; }
					SmoothMs.store(V, std::memory_order_relaxed);
					break;
				}
				default: break;
			}
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
			Smoother.Prepare(InSampleRate, SmoothMs.load(std::memory_order_relaxed));
			// Reset to Min so the first note ramps cleanly from the floor.
			Smoother.Reset(Min.load(std::memory_order_relaxed));
		}

		void Process(const FProcessContext& Ctx) override
		{
			float* Out = GetOutputBuffer(0);
			if (Out == nullptr) { return; }

			// Keep the smoother's time constant in sync if the user
			// dragged the Smooth slider since last block.
			Smoother.Prepare(SampleRate, SmoothMs.load(std::memory_order_relaxed));

			const float Lo = Min.load(std::memory_order_relaxed);
			const float Hi = Max.load(std::memory_order_relaxed);
			const float Raw = static_cast<float>(LastRaw.load(std::memory_order_relaxed));
			const float Target = Lo + (Raw / 127.0f) * (Hi - Lo);
			Smoother.SetTarget(Target);
			for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
			{
				Out[I] = Smoother.Tick();
			}
		}

		// Called from the audio thread by FMidiDeviceManager after draining
		// the audio CC ring. Filters by the node's assigned CC# and
		// Channel; on a match, updates the latched raw value.
		void OnCcEvent(uint8_t Channel, uint8_t Cc, uint8_t Value)
		{
			if (Cc != CcNumber.load(std::memory_order_relaxed)) { return; }
			const uint8_t Filter = ChannelFilter.load(std::memory_order_relaxed);
			if (Filter != 0 && Channel != Filter) { return; }
			LastRaw.store(Value, std::memory_order_relaxed);
		}

		// UI thread: latest raw 0..127 value received (for the "Last: N"
		// indicator in the property panel).
		uint8_t GetLastRaw() const
		{
			return LastRaw.load(std::memory_order_relaxed);
		}

	private:
		std::atomic<uint8_t> CcNumber{ 1 };
		std::atomic<uint8_t> ChannelFilter{ 0 };  // 0 = Omni, 1..16
		std::atomic<float> Min{ 0.0f };
		std::atomic<float> Max{ 1.0f };
		std::atomic<float> SmoothMs{ 5.0f };

		// Latched raw CC value. Written from the audio thread in OnCcEvent;
		// read by Process and the UI indicator.
		std::atomic<uint8_t> LastRaw{ 0 };

		double SampleRate = 48000.0;
		FOnePoleSmoother Smoother;
	};
}
