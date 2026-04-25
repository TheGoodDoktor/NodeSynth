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
		virtual void SetInputBuffer(uint32_t Index, const float* Buffer) = 0;
		virtual const float* GetInputBuffer(uint32_t Index) const = 0;
		virtual float* GetOutputBuffer(uint32_t Index) = 0;
	};

	// Convenience base that provides fixed-size input-pointer and output-buffer storage.
	// NumInputs/NumOutputs of 0 still reserves one slot to keep the array non-empty.
	template<uint32_t InNumInputs, uint32_t InNumOutputs>
	class TNodeBase : public INode
	{
	public:
		static constexpr uint32_t NumInputs = InNumInputs;
		static constexpr uint32_t NumOutputs = InNumOutputs;

		void SetInputBuffer(uint32_t Index, const float* Buffer) override
		{
			if (Index < NumInputs)
			{
				InputBuffers[Index] = Buffer;
			}
		}

		const float* GetInputBuffer(uint32_t Index) const override
		{
			return (Index < NumInputs) ? InputBuffers[Index] : nullptr;
		}

		float* GetOutputBuffer(uint32_t Index) override
		{
			return (Index < NumOutputs) ? OutputBuffers[Index] : nullptr;
		}

	protected:
		static constexpr uint32_t InputSlots = (NumInputs > 0) ? NumInputs : 1;
		static constexpr uint32_t OutputSlots = (NumOutputs > 0) ? NumOutputs : 1;

		const float* InputBuffers[InputSlots] = {};
		alignas(16) float OutputBuffers[OutputSlots][BlockSize] = {};
	};
}
