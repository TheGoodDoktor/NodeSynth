#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace NodeSynth
{
	// Compile-time block size. All nodes process exactly this many samples per
	// Process() call. Picked from the plan's Phase 1 "decisions to lock early".
	inline constexpr uint32_t BlockSize = 64;

	// Audio buffers are 2-channel (L/R). Control buffers reuse the same storage
	// shape but only channel 0 carries signal — control rate stays mono per the
	// Phase 5 design (`docs/PLAN-PHASE-5.md` §1.1). Phase 5b lands the buffer
	// widening + plumbing; existing mono nodes write to channel 0 only and the
	// graph compiler broadcasts a mono producer's channel 0 into both L and R
	// of every consumer (wire-level broadcast — no per-sample copy). True
	// stereo behaviour (Reverb, Delay, Chorus, Flanger) lands in 5c+.
	inline constexpr uint32_t NumChannels = 2;

	using FNodeId = uint64_t;
	using FLinkId = uint64_t;

	// Mirrors FVoiceAllocator::MaxVoices. Lives here too so opt-in
	// per-voice live-value arrays in DSP nodes can be sized at compile
	// time without including the voice allocator header.
	inline constexpr int32_t LiveMaxVoices = 8;

	enum class EPortType : uint8_t
	{
		Audio,
		Control,
	};

	struct FPortInfo
	{
		std::string Name;
		EPortType Type = EPortType::Audio;
		// Optional one-line tooltip shown on hover in the graph editor. Empty
		// disables. Trailing field so existing brace initialisers that omit it
		// still compile.
		std::string Description;
	};

	enum class EParamKind : uint8_t
	{
		Float,   // continuous slider, MinValue..MaxValue
		Choice,  // combo box, integer index into Choices[]
		Bool,    // checkbox, stored as 0.0f / 1.0f
		String,  // text field + optional file picker; not stored as a float.
		         // Float Get/SetParamValue are no-ops; use GetParamString /
		         // SetParamString instead. Patch save/load roundtrips the
		         // value via PatchSerializer's string-kind branch. Edit
		         // history doesn't track string params in v1.
	};

	struct FParamInfo
	{
		std::string Name;
		float MinValue = 0.0f;
		float MaxValue = 1.0f;
		float DefaultValue = 0.0f;
		bool bLogarithmic = false;
		EParamKind Kind = EParamKind::Float;
		std::vector<std::string> Choices;  // populated only when Kind == Choice
		// Optional one-line tooltip text shown on hover in the property panel.
		// Empty disables the tooltip. New field at the end so existing brace
		// initialisers that omit it default-construct to "".
		std::string Description;
		// When true, the standard property-panel widget loop skips this param.
		// Save/load still round-trips it. Use for params that have a custom UI
		// (e.g. the sequencer's per-step grid) so they don't appear twice.
		bool bHidden = false;
		// If non-negative, the input port index whose Control buffer overrides
		// this param at audio rate. The property panel uses this to (a) detect
		// whether the param is currently being modulated and disable the
		// slider, and (b) display the live effective value via
		// GetLiveParamValue(). Defaults to -1 (no Control input).
		int32_t ControlInputIndex = -1;
		// When true, the property panel renders this Float param as an
		// ImGui::DragFloat (typeable number with drag-to-scrub) instead of
		// the bar-style ImGui::SliderFloat. Use for params whose value is
		// arbitrary (Constant, Scale endpoints) where typing the exact
		// number is the natural workflow. Sliders are still the right call
		// for bounded musical params like cutoff or attack-ms.
		bool bUseInputBox = false;
	};

	struct FProcessContext
	{
		uint32_t BlockSize = NodeSynth::BlockSize;
		double SampleRate = 48000.0;
	};

	class INode
	{
	public:
		virtual ~INode() = default;

		virtual const char* GetTypeName() const = 0;

		virtual std::vector<FPortInfo> GetInputPorts() const = 0;
		virtual std::vector<FPortInfo> GetOutputPorts() const = 0;
		virtual std::vector<FParamInfo> GetParamInfos() const { return {}; }

		virtual float GetParamValue(uint32_t Index) const { (void)Index; return 0.0f; }
		virtual void SetParamValue(uint32_t Index, float Value) { (void)Index; (void)Value; }

		// Like GetParamValue, but returns the most recently computed effective
		// value when a Control input is overriding the param. Defaults to
		// GetParamValue — nodes that want their property-panel sliders to
		// reflect modulation override this and write the last block's value
		// to a small atomic at the end of Process. Read on the UI thread once
		// per frame; cost is negligible.
		virtual float GetLiveParamValue(uint32_t Index) const { return GetParamValue(Index); }

		// For per-voice nodes only. Compile sets this on every clone to point
		// at the master node (the one held by the UI-thread FNodeRecord). The
		// audio thread runs Process on the clones, never on the master, so
		// any live-display state (e.g. FOscillator::LastFrequency) the master
		// holds would never update. Opt-in nodes write their per-voice live
		// values into MasterMirror's per-voice arrays indexed by VoiceIndex.
		// Master nodes leave MasterMirror null; the default Clone in
		// NodeRegistry.cpp sets it on each clone.
		INode* MasterMirror = nullptr;

		// Voice index for per-voice clones, set by Compile (0..MaxVoices-1).
		// Master / mono nodes leave it at 0 — they always write slot 0 of
		// their own live-value arrays.
		int32_t VoiceIndex = 0;

		// String-kind param accessors. Default implementations return empty
		// / no-op so nodes that don't expose any String params don't need to
		// override. Used by FSidPlayer for the .sid file path. NOT RT-safe
		// — these methods may allocate, take filesystem locks, etc. Always
		// call from the UI thread; never from the audio callback.
		virtual std::string GetParamString(uint32_t Index) const { (void)Index; return {}; }
		virtual void SetParamString(uint32_t Index, const std::string& Value) { (void)Index; (void)Value; }

		virtual void Prepare(double SampleRate) { (void)SampleRate; }
		virtual void Process(const FProcessContext& Ctx) = 0;

		// Returns a fresh node of the same type with current param values copied
		// over. Transient DSP state (oscillator phase, ADSR stage, filter z's,
		// smoother current, etc.) resets when Prepare() is called on the clone.
		// Non-cloneable nodes (RtMidi-owning, UI-stateful, singleton sinks)
		// override to return nullptr. The default implementation lives in
		// ui/NodeRegistry.cpp because it needs the type-name → factory lookup;
		// nodes that can use the default don't need to override.
		virtual std::shared_ptr<INode> Clone() const;

		// Buffer routing. Set by graph compilation (UI thread), read during Process (audio thread).
		// Pointers remain valid for the lifetime of the compiled FAudioGraph snapshot that owns them.
		// Channel 0 = L, channel 1 = R. The default of 0 keeps existing single-channel callsites
		// reading/writing the L channel; stereo-aware nodes pass an explicit channel index.
		virtual void SetInputBuffer(uint32_t Index, const float* Buffer, uint32_t Channel = 0) = 0;
		virtual const float* GetInputBuffer(uint32_t Index, uint32_t Channel = 0) const = 0;
		virtual float* GetOutputBuffer(uint32_t Index, uint32_t Channel = 0) = 0;

		// True iff the node writes distinct content to channels 0 and 1 of the
		// named output port. Mono nodes return false (the default) and the
		// graph compiler broadcasts their L buffer onto downstream R inputs.
		// Stereo-aware nodes (Reverb, Delay, Chorus, Flanger) override and the
		// compiler plumbs L→L, R→R so the two streams stay separate.
		virtual bool IsOutputStereo(uint32_t Index) const { (void)Index; return false; }
	};

	// Convenience base that provides fixed-size input-pointer and output-buffer storage.
	// NumInputs/NumOutputs of 0 still reserves one slot to keep the array non-empty.
	// Buffers are 2-channel (L/R); mono nodes only ever write to channel 0 and the
	// graph compiler arranges for channel 1 of every consumer to alias channel 0 of
	// a mono producer (wire-level broadcast).
	template<uint32_t InNumInputs, uint32_t InNumOutputs>
	class TNodeBase : public INode
	{
	public:
		static constexpr uint32_t NumInputs = InNumInputs;
		static constexpr uint32_t NumOutputs = InNumOutputs;

		void SetInputBuffer(uint32_t Index, const float* Buffer, uint32_t Channel = 0) override
		{
			if (Index < NumInputs && Channel < NumChannels)
			{
				InputBuffers[Index][Channel] = Buffer;
			}
		}

		const float* GetInputBuffer(uint32_t Index, uint32_t Channel = 0) const override
		{
			return (Index < NumInputs && Channel < NumChannels) ? InputBuffers[Index][Channel] : nullptr;
		}

		float* GetOutputBuffer(uint32_t Index, uint32_t Channel = 0) override
		{
			return (Index < NumOutputs && Channel < NumChannels) ? OutputBuffers[Index][Channel] : nullptr;
		}

	protected:
		static constexpr uint32_t InputSlots = (NumInputs > 0) ? NumInputs : 1;
		static constexpr uint32_t OutputSlots = (NumOutputs > 0) ? NumOutputs : 1;

		const float* InputBuffers[InputSlots][NumChannels] = {};
		alignas(16) float OutputBuffers[OutputSlots][NumChannels][BlockSize] = {};
	};
}
