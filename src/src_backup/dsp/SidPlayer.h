#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	class FSidEmulator;

	// FSidPlayer plays back a Commodore 64 .sid (PSID v1/v2) tune using the
	// FSidEmulator backend, and exposes both the synthesized audio and the
	// SID register-write stream as outputs into the NodeSynth graph.
	//
	// Outputs (29 total; index in parens):
	//   (0)  Audio   "Out"             - mono SID audio (channel 0; channel 1
	//                                    stays zero and is broadcast by the
	//                                    Phase 5b wire-level R-aliases-L rule)
	//   Per voice (V1/V2/V3, indices 1..24):
	//     V*n*_Freq      Hz             - smoothed
	//     V*n*_PWM       0..1           - smoothed
	//     V*n*_Gate      0/1            - step
	//     V*n*_Waveform  bitmask 0..15  - step (Triangle=1 Saw=2 Pulse=4 Noise=8)
	//     V*n*_Attack    ms             - step
	//     V*n*_Decay     ms             - step
	//     V*n*_Sustain   0..1           - step
	//     V*n*_Release   ms             - step
	//   Globals (indices 25..28):
	//     F_Cutoff       0..1           - smoothed (intentionally normalised,
	//                                    not Hz; the 6581 cutoff curve is
	//                                    non-linear and chip-to-chip variable)
	//     F_Resonance    0..1           - smoothed
	//     F_Routing      bitmask 0..15  - step (V1=1 V2=2 V3=4 ExtIn=8)
	//     Volume         0..1           - smoothed
	//
	// Params:
	//   Param_File      String   .sid file path
	//   Param_Subtune   Float    1-based subtune index (clamped to file's count)
	//   Param_Region    Choice   PAL / NTSC (default from PSID header)
	//   Param_Model     Choice   6581 / 8580 (default from header; v1
	//                            informational only — m6581 emulates one curve)
	//   Param_Bypass    Bool     when true, audio + Control outputs hold zero
	//
	// Threading: file loading happens on the UI thread inside SetParamString.
	// The audio thread reads the loaded emulator via an atomic shared_ptr.
	class FSidPlayer : public INode
	{
	public:
		FSidPlayer();
		~FSidPlayer() override;

		FSidPlayer(const FSidPlayer&) = delete;
		FSidPlayer& operator=(const FSidPlayer&) = delete;

		const char* GetTypeName() const override { return "SidPlayer"; }

		std::vector<FPortInfo> GetInputPorts() const override { return {}; }
		std::vector<FPortInfo> GetOutputPorts() const override;
		std::vector<FParamInfo> GetParamInfos() const override;

		float GetParamValue(uint32_t Index) const override;
		void SetParamValue(uint32_t Index, float Value) override;
		std::string GetParamString(uint32_t Index) const override;
		void SetParamString(uint32_t Index, const std::string& Value) override;

		void Prepare(double SampleRate) override;
		void Process(const FProcessContext& Ctx) override;

		// Buffer routing — TNodeBase-style storage but inline here so we can
		// describe the per-output channel-1 broadcast convention.
		void SetInputBuffer(uint32_t /*Index*/, const float* /*Buffer*/, uint32_t /*Channel*/ = 0) override {}
		const float* GetInputBuffer(uint32_t /*Index*/, uint32_t /*Channel*/ = 0) const override { return nullptr; }
		float* GetOutputBuffer(uint32_t Index, uint32_t Channel = 0) override;

		// Public param indices.
		enum EParam : uint32_t
		{
			Param_File = 0,
			Param_Subtune,
			Param_Region,
			Param_Model,
			Param_Bypass,
			Param_COUNT,
		};

		// Output port indices.
		enum EOutput : uint32_t
		{
			Output_Audio = 0,
			// Per-voice outputs in 8-wide blocks.
			Output_V1_Freq, Output_V1_PWM, Output_V1_Gate, Output_V1_Waveform,
			Output_V1_Attack, Output_V1_Decay, Output_V1_Sustain, Output_V1_Release,
			Output_V2_Freq, Output_V2_PWM, Output_V2_Gate, Output_V2_Waveform,
			Output_V2_Attack, Output_V2_Decay, Output_V2_Sustain, Output_V2_Release,
			Output_V3_Freq, Output_V3_PWM, Output_V3_Gate, Output_V3_Waveform,
			Output_V3_Attack, Output_V3_Decay, Output_V3_Sustain, Output_V3_Release,
			// Globals.
			Output_F_Cutoff,
			Output_F_Resonance,
			Output_F_Routing,
			Output_Volume,
			Output_COUNT,
		};
		static_assert(Output_COUNT == 29, "Output indices must enumerate 1 audio + 28 control");

		// Per-voice cloning is disallowed: the emulator state is heavy and
		// running 8 copies of the same SID tune in parallel makes no sense.
		std::shared_ptr<INode> Clone() const override { return nullptr; }

		// Test / inspection.
		struct FLoadStatus
		{
			bool bLoaded = false;
			std::string ErrorMessage;
			std::string TuneName;
			std::string Author;
			std::string Released;
			uint16_t Songs = 0;
			uint16_t StartSong = 0;
			bool bIsNtsc = false;
			bool bIs8580 = false;
		};
		FLoadStatus GetStatus() const;

		// Live gate state for voice 0..2 (corresponds to SID voices V1..V3).
		// Reads the latched LastValue mirror — racy with the audio thread's
		// Process loop, but the worst case is a one-frame stale reading,
		// which is fine for a UI indicator. Returns false on out-of-range.
		bool GetVoiceGate(uint32_t Voice) const;

	private:
		// Per-output buffer storage. Channel 0 carries content; channel 1 stays
		// zero (mono node — wire broadcast handles downstream stereo consumers).
		alignas(16) float OutputBuffers[Output_COUNT][NumChannels][BlockSize] = {};

		// Smoothed-class outputs (Freq×3, PWM×3, Cutoff, Resonance, Volume) get
		// per-output one-pole smoothers so register writes that update at PSID
		// tick rate (50/60 Hz) don't zipper at audio rate. Indices align with
		// Output_*: 0 = unused (audio), 1+ = control. Mostly only used for the
		// smoothed entries; step-class outputs hold the raw float and ignore
		// the smoother.
		FOnePoleSmoother Smoothers[Output_COUNT];
		// Last-known register-derived value for each output. Updated on every
		// captured write; held forever for step outputs, used as the smoother
		// target for smoothed outputs.
		float LastValue[Output_COUNT] = {};

		// Shadow of the last byte written to each register, per voice (7
		// registers per voice) and global (4 entries: cutoff lo/hi, res-routing,
		// mode-volume). Lets us decode pairs of register writes (e.g. freg lo
		// + hi) into a single derived value without peeking the SID's state.
		uint8_t CachedRegs[3][7] = {};
		uint8_t CachedGlobals[4] = {};

		// Latest engine snapshot, atomic-swapped on file load. Audio thread
		// loads once per Process; UI thread builds + stores.
		std::shared_ptr<FSidEmulator> ActiveEmulator{ nullptr };

		// Parsed-tune state held alongside the emulator. Captured during file
		// load and mirrored into Status for the UI panel.
		struct FTuneInfo
		{
			std::string Name;
			std::string Author;
			std::string Released;
			uint16_t Songs = 0;
			uint16_t StartSong = 0;
			bool bIsNtsc = false;
			bool bIs8580 = false;
			double ChipClockHz = 985248.0;
		};
		// Guarded by InfoMutex (UI-thread reads/writes for the property panel;
		// the audio thread doesn't touch it).
		mutable std::mutex InfoMutex;
		FTuneInfo Info;
		std::string FilePath;     // current Param_File value
		std::string LoadError;    // empty on success
		// Param state (UI-thread writes; audio reads via the SetParam queue,
		// or directly for non-RT params).
		std::atomic<float> ParamSubtune{ 1.0f };
		std::atomic<float> ParamRegion{ 0.0f };  // 0 = PAL, 1 = NTSC
		std::atomic<float> ParamModel{ 0.0f };   // 0 = 6581, 1 = 8580
		std::atomic<bool>  ParamBypass{ false };

		double SampleRate = 48000.0;

		// Reload the file from FilePath using the current Subtune/Region/Model
		// params; build a fresh FSidEmulator, run init, atomic-swap into
		// ActiveEmulator. Called from SetParamString and SetParamValue (when
		// Subtune/Region/Model changes).
		void RebuildEmulator();

		// Process body: tick the active emulator one audio sample at a time,
		// dispatch register writes captured during each tick into the Control
		// output buffers, fill audio output. Caller reserves Writes capacity.
		void TickBlock(const FProcessContext& Ctx);
	};
}
