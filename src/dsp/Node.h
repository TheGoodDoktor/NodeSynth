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
