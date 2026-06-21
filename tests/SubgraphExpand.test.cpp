#include "dsp/Constant.h"
#include "dsp/Gain.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"
#include "dsp/Subgraph.h"
#include "dsp/Svf.h"
#include "dsp/internal/SubgraphBoundary.h"
#include "graph/Graph.h"
#include "graph/SubgraphDefinition.h"
#include "graph/SubgraphOps.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <memory>
#include <string>

using Catch::Matchers::WithinAbs;
using NodeSynth::EPortType;
using NodeSynth::FConstant;
using NodeSynth::FGain;
using NodeSynth::FGraphModel;
using NodeSynth::FNodeId;
using NodeSynth::FOscillator;
using NodeSynth::FOutput;
using NodeSynth::FProcessContext;
using NodeSynth::FSubgraph;
using NodeSynth::FSubgraphDefinition;
using NodeSynth::FSvf;
using NodeSynth::GroupNodesIntoSubgraph;
using NodeSynth::SyncSubgraphBoundaries;
namespace Internal = NodeSynth::Internal;

namespace
{
	// "GainBox": In (Audio) -> Gain -> Out (Audio). Exercises both the input-pin
	// and output-pin rewiring paths during expansion.
	std::shared_ptr<FSubgraphDefinition> MakeGainBox(float GainValue)
	{
		auto Def = std::make_shared<FSubgraphDefinition>();
		Def->Name = "GainBox";
		Def->InputPins = { { "In", EPortType::Audio, "" } };
		Def->OutputPins = { { "Out", EPortType::Audio, "" } };

		auto Inputs = std::make_shared<Internal::FSubgraphInputs>();
		auto Outputs = std::make_shared<Internal::FSubgraphOutputs>();
		auto GainN = std::make_shared<FGain>();
		GainN->SetParamValue(FGain::Param_Gain, GainValue);

		const FNodeId InId = Def->InternalGraph.AddNode(Inputs);
		const FNodeId OutId = Def->InternalGraph.AddNode(Outputs);
		const FNodeId GId = Def->InternalGraph.AddNode(GainN);

		// Boundary ports must exist before linking (AddLink validates types).
		SyncSubgraphBoundaries(*Def);
		REQUIRE(Def->InternalGraph.AddLink(InId, 0, GId, 0) != 0);
		REQUIRE(Def->InternalGraph.AddLink(GId, 0, OutId, 0) != 0);
		return Def;
	}
}

TEST_CASE("Subgraph expansion: a subgraph produces audio identical to the inlined graph", "[subgraph][expand]")
{
	constexpr float Freq = 220.0f;
	constexpr float GainValue = 0.5f;

	// Reference: Osc -> Gain -> Output, hand-wired.
	FGraphModel Ref;
	auto RefOsc = std::make_shared<FOscillator>();
	auto RefGain = std::make_shared<FGain>();
	auto RefOut = std::make_shared<FOutput>();
	RefOsc->SetParamValue(FOscillator::Param_Frequency, Freq);
	RefGain->SetParamValue(FGain::Param_Gain, GainValue);
	const FNodeId RefOscId = Ref.AddNode(RefOsc);
	const FNodeId RefGainId = Ref.AddNode(RefGain);
	const FNodeId RefOutId = Ref.AddNode(RefOut);
	REQUIRE(Ref.AddLink(RefOscId, 0, RefGainId, 0) != 0);
	REQUIRE(Ref.AddLink(RefGainId, 0, RefOutId, 0) != 0);

	// Subgraph: Osc -> [GainBox] -> Output.
	FGraphModel Sub;
	auto SubOsc = std::make_shared<FOscillator>();
	auto SubNode = std::make_shared<FSubgraph>();
	auto SubOut = std::make_shared<FOutput>();
	SubOsc->SetParamValue(FOscillator::Param_Frequency, Freq);
	SubNode->SetDefinition(MakeGainBox(GainValue));
	const FNodeId SubOscId = Sub.AddNode(SubOsc);
	const FNodeId SubId = Sub.AddNode(SubNode);
	const FNodeId SubOutId = Sub.AddNode(SubOut);
	REQUIRE(Sub.AddLink(SubOscId, 0, SubId, 0) != 0);
	REQUIRE(Sub.AddLink(SubId, 0, SubOutId, 0) != 0);

	auto RefSnap = Ref.Compile(48000.0);
	auto SubSnap = Sub.Compile(48000.0);
	REQUIRE_FALSE(RefSnap->OrderedNodes.empty());
	REQUIRE_FALSE(SubSnap->OrderedNodes.empty());

	// The expanded snapshot must contain no Subgraph / boundary nodes — just
	// the flattened Osc + Gain + Output, same as the reference.
	for (const auto& N : SubSnap->OrderedNodes)
	{
		const std::string T = N->GetTypeName();
		REQUIRE(T != "Subgraph");
		REQUIRE(T != "_SubgraphInputs");
		REQUIRE(T != "_SubgraphOutputs");
	}
	REQUIRE(SubSnap->OrderedNodes.size() == RefSnap->OrderedNodes.size());

	FProcessContext Ctx;
	Ctx.SampleRate = 48000.0;

	// Process several blocks and compare the buffer feeding each Output.
	for (int Block = 0; Block < 4; ++Block)
	{
		RefSnap->Process(Ctx);
		SubSnap->Process(Ctx);

		const float* RefBuf = RefOut->GetInputBuffer(0);
		const float* SubBuf = SubOut->GetInputBuffer(0);
		REQUIRE(RefBuf != nullptr);
		REQUIRE(SubBuf != nullptr);
		for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
		{
			REQUIRE_THAT(SubBuf[I], WithinAbs(RefBuf[I], 1e-6f));
		}
	}
}

TEST_CASE("Subgraph expansion: a forbidden internal node type is rejected", "[subgraph][expand]")
{
	auto Def = std::make_shared<FSubgraphDefinition>();
	Def->Name = "BadBox";
	Def->OutputPins = { { "Out", EPortType::Audio, "" } };
	// An FOutput inside a subgraph is forbidden — it's the patch sink.
	Def->InternalGraph.AddNode(std::make_shared<FOutput>());

	FGraphModel Model;
	auto SubNode = std::make_shared<FSubgraph>();
	SubNode->SetDefinition(Def);
	auto Out = std::make_shared<FOutput>();
	Model.AddNode(SubNode);
	Model.AddNode(Out);

	auto Snap = Model.Compile(48000.0);
	REQUIRE(Snap->OrderedNodes.empty());
	REQUIRE(Model.GetLastCompileError().bHasError);
	REQUIRE(Model.GetLastCompileError().Message.find("forbidden") != std::string::npos);
}

TEST_CASE("Subgraph pins: RemovePortAndShiftLinks drops the pin and shifts higher ports", "[subgraph][pins]")
{
	// Mirror a boundary node with three output ports feeding three consumers,
	// then remove the middle pin as the pin panel would.
	FGraphModel M;
	auto Inputs = std::make_shared<Internal::FSubgraphInputs>();
	Inputs->SetPorts({ { "A", EPortType::Audio, "" },
		{ "B", EPortType::Audio, "" },
		{ "C", EPortType::Audio, "" } });
	auto G0 = std::make_shared<FGain>();
	auto G1 = std::make_shared<FGain>();
	auto G2 = std::make_shared<FGain>();
	const FNodeId InId = M.AddNode(Inputs);
	const FNodeId I0 = M.AddNode(G0);
	const FNodeId I1 = M.AddNode(G1);
	const FNodeId I2 = M.AddNode(G2);
	REQUIRE(M.AddLink(InId, 0, I0, 0) != 0);
	REQUIRE(M.AddLink(InId, 1, I1, 0) != 0);
	REQUIRE(M.AddLink(InId, 2, I2, 0) != 0);
	REQUIRE(M.CountPortLinks(InId, /*bOutputPort*/ true, 1) == 1);

	M.RemovePortAndShiftLinks(InId, /*bOutputPort*/ true, 1);

	// Port 0 stays on G0; old port 2 (→ G2) shifts down to port 1; no port 2.
	REQUIRE(M.CountPortLinks(InId, true, 0) == 1);
	REQUIRE(M.CountPortLinks(InId, true, 1) == 1);
	REQUIRE(M.CountPortLinks(InId, true, 2) == 0);
	bool ToG0 = false;
	bool ToG2 = false;
	for (const auto& L : M.GetLinks())
	{
		if (L.FromNode == InId && L.FromPort == 0 && L.ToNode == I0) { ToG0 = true; }
		if (L.FromNode == InId && L.FromPort == 1 && L.ToNode == I2) { ToG2 = true; }
	}
	REQUIRE(ToG0);
	REQUIRE(ToG2);
}

TEST_CASE("Subgraph pins: SwapPortLinks exchanges two ports' links", "[subgraph][pins]")
{
	FGraphModel M;
	auto Outputs = std::make_shared<Internal::FSubgraphOutputs>();
	Outputs->SetPorts({ { "X", EPortType::Audio, "" }, { "Y", EPortType::Audio, "" } });
	auto P0 = std::make_shared<FGain>();
	auto P1 = std::make_shared<FGain>();
	const FNodeId OutId = M.AddNode(Outputs);
	const FNodeId N0 = M.AddNode(P0);
	const FNodeId N1 = M.AddNode(P1);
	// Producers feed the OutputsBoundary's input ports 0 and 1.
	REQUIRE(M.AddLink(N0, 0, OutId, 0) != 0);
	REQUIRE(M.AddLink(N1, 0, OutId, 1) != 0);

	M.SwapPortLinks(OutId, /*bOutputPort*/ false, 0, 1);

	bool Port0FromN1 = false;
	bool Port1FromN0 = false;
	for (const auto& L : M.GetLinks())
	{
		if (L.ToNode == OutId && L.ToPort == 0 && L.FromNode == N1) { Port0FromN1 = true; }
		if (L.ToNode == OutId && L.ToPort == 1 && L.FromNode == N0) { Port1FromN0 = true; }
	}
	REQUIRE(Port0FromN1);
	REQUIRE(Port1FromN0);
}

TEST_CASE("Subgraph expansion: transitive recursion is rejected", "[subgraph][expand]")
{
	// A definition that contains an instance of itself. This necessarily forms
	// a shared_ptr cycle (the definition owns a node that owns the definition);
	// the test process leaks it on exit, which is acceptable for a unit test.
	auto Def = std::make_shared<FSubgraphDefinition>();
	Def->Name = "Recursive";
	Def->OutputPins = { { "Out", EPortType::Audio, "" } };
	auto Inner = std::make_shared<FSubgraph>();
	Inner->SetDefinition(Def);  // self-reference
	Def->InternalGraph.AddNode(Inner);

	FGraphModel Model;
	auto SubNode = std::make_shared<FSubgraph>();
	SubNode->SetDefinition(Def);
	auto Out = std::make_shared<FOutput>();
	Model.AddNode(SubNode);
	Model.AddNode(Out);

	auto Snap = Model.Compile(48000.0);
	REQUIRE(Snap->OrderedNodes.empty());
	REQUIRE(Model.GetLastCompileError().bHasError);
	REQUIRE(Model.GetLastCompileError().Message.find("recursive") != std::string::npos);
}

namespace
{
	// Builds: Osc -> Gain -> SVF.Audio, Const -> SVF.Cutoff, SVF.LP -> Output.
	// Returns the three groupable node ids and the Output node pointer.
	struct FGroupFixture
	{
		FNodeId OscId = 0, GainId = 0, SvfId = 0;
		std::shared_ptr<FOutput> Out;
	};
	FGroupFixture BuildGroupFixture(FGraphModel& M)
	{
		FGroupFixture F;
		auto Osc = std::make_shared<FOscillator>();
		auto Gain = std::make_shared<FGain>();
		auto Svf = std::make_shared<FSvf>();
		auto Const = std::make_shared<FConstant>();
		F.Out = std::make_shared<FOutput>();
		Osc->SetParamValue(FOscillator::Param_Frequency, 220.0f);
		Gain->SetParamValue(FGain::Param_Gain, 0.7f);
		Svf->SetParamValue(FSvf::Param_Resonance, 0.5f);
		Const->SetParamValue(FConstant::Param_Value, 900.0f);

		F.OscId = M.AddNode(Osc, 0.0f, 0.0f);
		F.GainId = M.AddNode(Gain, 100.0f, 0.0f);
		F.SvfId = M.AddNode(Svf, 200.0f, 0.0f);
		const FNodeId ConstId = M.AddNode(Const, 100.0f, 200.0f);
		const FNodeId OutId = M.AddNode(F.Out, 400.0f, 0.0f);
		M.AddLink(F.OscId, 0, F.GainId, 0);
		M.AddLink(F.GainId, 0, F.SvfId, FSvf::Input_Audio);
		M.AddLink(ConstId, 0, F.SvfId, FSvf::Input_Cutoff);
		M.AddLink(F.SvfId, FSvf::Output_LowPass, OutId, 0);
		return F;
	}
}

TEST_CASE("Make subgraph from selection: groups 3 nodes, promotes pins, stays audio-identical",
	"[subgraph][group]")
{
	// Reference (never grouped) and target (grouped) built identically.
	FGraphModel Ref;
	FGroupFixture RefF = BuildGroupFixture(Ref);

	FGraphModel Target;
	FGroupFixture TgtF = BuildGroupFixture(Target);

	const FNodeId InstId = GroupNodesIntoSubgraph(Target,
		{ TgtF.OscId, TgtF.GainId, TgtF.SvfId });
	REQUIRE(InstId != 0);

	// The three originals are gone, replaced by one Subgraph instance.
	REQUIRE(Target.FindNode(TgtF.OscId) == nullptr);
	REQUIRE(Target.FindNode(TgtF.GainId) == nullptr);
	REQUIRE(Target.FindNode(TgtF.SvfId) == nullptr);
	{
		NodeSynth::FNodeRecord* Inst = Target.FindNode(InstId);
		REQUIRE(Inst != nullptr);
		auto* Sub = dynamic_cast<FSubgraph*>(Inst->Node.get());
		REQUIRE(Sub != nullptr);
		REQUIRE(Sub->GetDefinition() != nullptr);
		// One crossing input (Const -> SVF.Cutoff) and one crossing output
		// (SVF.LP -> Output) were promoted to pins.
		REQUIRE(Sub->GetDefinition()->InputPins.size() == 1);
		REQUIRE(Sub->GetDefinition()->OutputPins.size() == 1);
		REQUIRE(Sub->GetInputPorts().size() == 1);
		REQUIRE(Sub->GetOutputPorts().size() == 1);
	}

	auto RefSnap = Ref.Compile(48000.0);
	auto TgtSnap = Target.Compile(48000.0);
	REQUIRE_FALSE(RefSnap->OrderedNodes.empty());
	REQUIRE_FALSE(TgtSnap->OrderedNodes.empty());

	FProcessContext Ctx;
	Ctx.SampleRate = 48000.0;
	for (int Block = 0; Block < 4; ++Block)
	{
		RefSnap->Process(Ctx);
		TgtSnap->Process(Ctx);
		const float* RefBuf = RefF.Out->GetInputBuffer(0);
		const float* TgtBuf = TgtF.Out->GetInputBuffer(0);
		REQUIRE(RefBuf != nullptr);
		REQUIRE(TgtBuf != nullptr);
		for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
		{
			REQUIRE_THAT(TgtBuf[I], WithinAbs(RefBuf[I], 1e-6f));
		}
	}
}
