#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "dsp/Node.h"
#include "dsp/Smoother.h"

namespace NodeSynth
{
	// 8 × 8 modulation matrix. Eight Control inputs feed eight Control
	// outputs via a per-cell depth-weighted sum plus a per-output offset:
	//
	//   Dst_i[s] = Offset_i + Σ_j (Src_j[s] × Depth_ij)
	//
	// Replaces the LFO → Scale → Add chains that pile up on heavily
	// modulated patches. Per-output smoother (30 ms) hides depth-knob
	// drag clicks. All 72 params are hidden — the custom UI in
	// ui/ModMatrixUI.cpp surfaces them as an 8×8 grid + offset row.
	//
	// See docs/PLAN-MOD-MATRIX.md.
	class FModulationMatrix : public TNodeBase<8, 8>
	{
	public:
		static constexpr uint32_t NumSources = 8;
		static constexpr uint32_t NumDestinations = 8;
		static constexpr uint32_t NumDepths = NumSources * NumDestinations;

		enum EParam : uint32_t
		{
			Param_DepthBase  = 0,
			Param_OffsetBase = NumDepths,                  // 64
			Param_COUNT      = NumDepths + NumDestinations, // 72
		};

		FModulationMatrix()
		{
			for (uint32_t I = 0; I < NumDepths; ++I)
			{
				DepthValues[I].store(0.0f, std::memory_order_relaxed);
			}
			for (uint32_t I = 0; I < NumDestinations; ++I)
			{
				OffsetValues[I].store(0.0f, std::memory_order_relaxed);
			}
		}

		const char* GetTypeName() const override { return "ModulationMatrix"; }

		std::vector<FPortInfo> GetInputPorts() const override
		{
			std::vector<FPortInfo> Ports;
			Ports.reserve(NumSources);
			for (uint32_t J = 0; J < NumSources; ++J)
			{
				FPortInfo P;
				P.Name = "Src" + std::to_string(J + 1);
				P.Type = EPortType::Control;
				P.Description = "Control source " + std::to_string(J + 1)
					+ ". Each cell in this column scales the source by its\n"
					"depth knob and adds it to the destination row's output.";
				Ports.push_back(std::move(P));
			}
			return Ports;
		}

		std::vector<FPortInfo> GetOutputPorts() const override
		{
			std::vector<FPortInfo> Ports;
			Ports.reserve(NumDestinations);
			for (uint32_t I = 0; I < NumDestinations; ++I)
			{
				FPortInfo P;
				P.Name = "Dst" + std::to_string(I + 1);
				P.Type = EPortType::Control;
				P.Description = "Destination " + std::to_string(I + 1)
					+ " — depth-weighted sum of inputs plus the row offset.";
				Ports.push_back(std::move(P));
			}
			return Ports;
		}

		std::vector<FParamInfo> GetParamInfos() const override
		{
			std::vector<FParamInfo> Out;
			Out.reserve(Param_COUNT);
			// Depth_<row>_<col>. row = destination, col = source.
			for (uint32_t I = 0; I < NumDestinations; ++I)
			{
				for (uint32_t J = 0; J < NumSources; ++J)
				{
					FParamInfo P{};
					P.Name = "Depth_" + std::to_string(I) + "_" + std::to_string(J);
					P.MinValue = -1.0f;
					P.MaxValue = 1.0f;
					P.DefaultValue = 0.0f;
					P.Kind = EParamKind::Float;
					P.bHidden = true;  // surfaced via custom UI grid
					Out.push_back(std::move(P));
				}
			}
			for (uint32_t I = 0; I < NumDestinations; ++I)
			{
				FParamInfo P{};
				P.Name = "Offset_" + std::to_string(I);
				P.MinValue = -1.0f;
				P.MaxValue = 1.0f;
				P.DefaultValue = 0.0f;
				P.Kind = EParamKind::Float;
				P.bHidden = true;
				Out.push_back(std::move(P));
			}
			return Out;
		}

		float GetParamValue(uint32_t Index) const override
		{
			if (Index < NumDepths)
			{
				return DepthValues[Index].load(std::memory_order_relaxed);
			}
			if (Index < Param_COUNT)
			{
				return OffsetValues[Index - NumDepths].load(std::memory_order_relaxed);
			}
			return 0.0f;
		}

		void SetParamValue(uint32_t Index, float Value) override
		{
			float V = Value;
			if (V < -1.0f) { V = -1.0f; }
			if (V > 1.0f)  { V = 1.0f; }
			if (Index < NumDepths)
			{
				DepthValues[Index].store(V, std::memory_order_relaxed);
				return;
			}
			if (Index < Param_COUNT)
			{
				OffsetValues[Index - NumDepths].store(V, std::memory_order_relaxed);
			}
		}

		void Prepare(double InSampleRate) override
		{
			SampleRate = InSampleRate;
			for (uint32_t I = 0; I < NumDestinations; ++I)
			{
				Smoothers[I].Prepare(InSampleRate, 30.0f);
				Smoothers[I].Reset(OffsetValues[I].load(std::memory_order_relaxed));
			}
		}

		void Process(const FProcessContext& Ctx) override
		{
			// Snapshot params once per block — depth/offset edits during the
			// block read uniformly instead of mid-loop drift.
			float Depth[NumDestinations][NumSources];
			float Offset[NumDestinations];
			for (uint32_t I = 0; I < NumDestinations; ++I)
			{
				Offset[I] = OffsetValues[I].load(std::memory_order_relaxed);
				for (uint32_t J = 0; J < NumSources; ++J)
				{
					Depth[I][J] = DepthValues[I * NumSources + J]
						.load(std::memory_order_relaxed);
				}
			}

			const float* SrcBuf[NumSources];
			for (uint32_t J = 0; J < NumSources; ++J)
			{
				SrcBuf[J] = GetInputBuffer(J);
			}
			float* DstBuf[NumDestinations];
			for (uint32_t I = 0; I < NumDestinations; ++I)
			{
				DstBuf[I] = GetOutputBuffer(I);
			}

			for (uint32_t S = 0; S < Ctx.BlockSize; ++S)
			{
				for (uint32_t I = 0; I < NumDestinations; ++I)
				{
					float Sum = Offset[I];
					for (uint32_t J = 0; J < NumSources; ++J)
					{
						if (SrcBuf[J] != nullptr)
						{
							Sum += SrcBuf[J][S] * Depth[I][J];
						}
					}
					Smoothers[I].SetTarget(Sum);
					DstBuf[I][S] = Smoothers[I].Tick();
				}
			}
		}

		// Convenience for tests / UI: index helpers.
		static constexpr uint32_t DepthIndex(uint32_t Dest, uint32_t Src)
		{
			return Dest * NumSources + Src;
		}
		static constexpr uint32_t OffsetIndex(uint32_t Dest)
		{
			return NumDepths + Dest;
		}

	private:
		std::atomic<float> DepthValues[NumDepths];
		std::atomic<float> OffsetValues[NumDestinations];
		FOnePoleSmoother Smoothers[NumDestinations];
		double SampleRate = 48000.0;
	};
}
