#include "dsp/Gain.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"
#include "graph/EditHistory.h"
#include "graph/Graph.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using NodeSynth::FEditCommand;
using NodeSynth::FEditHistory;
using NodeSynth::FGain;
using NodeSynth::FGraphModel;
using NodeSynth::FNodeId;
using NodeSynth::FOscillator;
using NodeSynth::FOutput;

TEST_CASE("EditHistory: AddNode → Undo removes the node", "[edithistory]")
{
	FGraphModel Model;
	FEditHistory History;
	Model.SetHistory(&History);

	const FNodeId Id = Model.AddNode(std::make_shared<FGain>(), 10.0f, 20.0f);
	REQUIRE(Model.GetNodes().size() == 1);
	REQUIRE(History.UndoSize() == 1);

	REQUIRE(History.Undo(Model));
	REQUIRE(Model.GetNodes().size() == 0);
	REQUIRE(History.UndoSize() == 0);
	REQUIRE(History.RedoSize() == 1);
	(void)Id;
}

TEST_CASE("EditHistory: Undo + Redo round-trips an AddNode", "[edithistory]")
{
	FGraphModel Model;
	FEditHistory History;
	Model.SetHistory(&History);

	const FNodeId Id = Model.AddNode(std::make_shared<FGain>(), 10.0f, 20.0f);
	History.Undo(Model);
	REQUIRE(Model.GetNodes().size() == 0);
	REQUIRE(History.Redo(Model));
	REQUIRE(Model.GetNodes().count(Id) == 1);
}

TEST_CASE("EditHistory: RemoveNode → Undo restores params + per-voice + links", "[edithistory]")
{
	FGraphModel Model;
	FEditHistory History;
	Model.SetHistory(&History);

	const FNodeId OscId = Model.AddNode(std::make_shared<FOscillator>(), 0.0f, 0.0f);
	const FNodeId OutId = Model.AddNode(std::make_shared<FOutput>(), 100.0f, 0.0f);
	Model.AddLink(OscId, 0, OutId, 0);

	auto* OscRec = Model.FindNode(OscId);
	REQUIRE(OscRec != nullptr);
	OscRec->Node->SetParamValue(FOscillator::Param_Frequency, 333.0f);
	REQUIRE(Model.SetNodePerVoice(OscId, true));

	REQUIRE(Model.GetLinks().size() == 1);
	Model.RemoveNode(OscId);
	REQUIRE(Model.GetNodes().count(OscId) == 0);
	REQUIRE(Model.GetLinks().size() == 0);

	REQUIRE(History.Undo(Model));
	REQUIRE(Model.GetNodes().count(OscId) == 1);
	REQUIRE(Model.GetLinks().size() == 1);
	auto& RestoredRec = Model.GetNodes().at(OscId);
	REQUIRE(RestoredRec.bPerVoice);
	REQUIRE_THAT(RestoredRec.Node->GetParamValue(FOscillator::Param_Frequency),
		Catch::Matchers::WithinAbs(333.0f, 1e-3f));
}

TEST_CASE("EditHistory: SetParam captures old + new values for round-trip", "[edithistory]")
{
	FGraphModel Model;
	FEditHistory History;

	const FNodeId Id = Model.AddNode(std::make_shared<FGain>());
	auto& Node = Model.GetNodes().at(Id).Node;
	Node->SetParamValue(FGain::Param_Gain, 0.5f);

	// Hook history AFTER initial setup so the AddNode entry doesn't pollute.
	Model.SetHistory(&History);
	History.Clear();

	// User-style param edit: build a SetParam edit-history entry by hand
	// (Editor.cpp does this in its slider-deactivate path).
	FEditCommand Cmd;
	Cmd.Type = NodeSynth::EEditCommand::SetParam;
	Cmd.NodeId = Id;
	Cmd.ParamIndex = FGain::Param_Gain;
	Cmd.OldValue = 0.5f;
	Cmd.NewValue = 1.5f;
	Node->SetParamValue(FGain::Param_Gain, 1.5f);
	History.Push(Cmd);

	REQUIRE_THAT(Node->GetParamValue(FGain::Param_Gain),
		Catch::Matchers::WithinAbs(1.5f, 1e-6f));

	History.Undo(Model);
	REQUIRE_THAT(Node->GetParamValue(FGain::Param_Gain),
		Catch::Matchers::WithinAbs(0.5f, 1e-6f));

	History.Redo(Model);
	REQUIRE_THAT(Node->GetParamValue(FGain::Param_Gain),
		Catch::Matchers::WithinAbs(1.5f, 1e-6f));
}

TEST_CASE("EditHistory: AddLink → Undo removes the link, Redo restores with same id", "[edithistory]")
{
	FGraphModel Model;
	FEditHistory History;
	Model.SetHistory(&History);

	const FNodeId OscId = Model.AddNode(std::make_shared<FOscillator>());
	const FNodeId OutId = Model.AddNode(std::make_shared<FOutput>());
	const NodeSynth::FLinkId LinkId = Model.AddLink(OscId, 0, OutId, 0);
	REQUIRE(LinkId != 0);
	REQUIRE(Model.GetLinks().size() == 1);

	REQUIRE(History.Undo(Model));
	REQUIRE(Model.GetLinks().size() == 0);

	REQUIRE(History.Redo(Model));
	REQUIRE(Model.GetLinks().size() == 1);
	REQUIRE(Model.GetLinks().front().Id == LinkId);
}

TEST_CASE("EditHistory: AddLink that displaces an existing link restores it on Undo", "[edithistory][displace]")
{
	FGraphModel Model;
	FEditHistory History;
	Model.SetHistory(&History);

	const FNodeId OscA = Model.AddNode(std::make_shared<FOscillator>());
	const FNodeId OscB = Model.AddNode(std::make_shared<FOscillator>());
	const FNodeId OutId = Model.AddNode(std::make_shared<FOutput>());

	const NodeSynth::FLinkId LinkA = Model.AddLink(OscA, 0, OutId, 0);
	REQUIRE(LinkA != 0);
	REQUIRE(Model.GetLinks().size() == 1);

	// Connecting OscB to the same input port displaces LinkA.
	const NodeSynth::FLinkId LinkB = Model.AddLink(OscB, 0, OutId, 0);
	REQUIRE(LinkB != 0);
	REQUIRE(LinkB != LinkA);
	REQUIRE(Model.GetLinks().size() == 1);
	REQUIRE(Model.GetLinks().front().Id == LinkB);
	REQUIRE(Model.GetLinks().front().FromNode == OscB);

	// One undo should restore the displaced LinkA, not just remove LinkB.
	REQUIRE(History.Undo(Model));
	REQUIRE(Model.GetLinks().size() == 1);
	REQUIRE(Model.GetLinks().front().Id == LinkA);
	REQUIRE(Model.GetLinks().front().FromNode == OscA);

	// Another undo removes the original AddLink — back to no links.
	REQUIRE(History.Undo(Model));
	REQUIRE(Model.GetLinks().size() == 0);

	// Redo brings LinkA back, then redo again displaces it for LinkB.
	REQUIRE(History.Redo(Model));
	REQUIRE(Model.GetLinks().size() == 1);
	REQUIRE(Model.GetLinks().front().Id == LinkA);
	REQUIRE(History.Redo(Model));
	REQUIRE(Model.GetLinks().size() == 1);
	REQUIRE(Model.GetLinks().front().Id == LinkB);
}

TEST_CASE("EditHistory: SetNodePerVoice round-trips through Undo + Redo", "[edithistory]")
{
	FGraphModel Model;
	FEditHistory History;
	Model.SetHistory(&History);

	const FNodeId Id = Model.AddNode(std::make_shared<FOscillator>());
	History.Clear();

	REQUIRE(Model.SetNodePerVoice(Id, true));
	REQUIRE(Model.GetNodes().at(Id).bPerVoice);

	History.Undo(Model);
	REQUIRE_FALSE(Model.GetNodes().at(Id).bPerVoice);

	History.Redo(Model);
	REQUIRE(Model.GetNodes().at(Id).bPerVoice);
}

TEST_CASE("EditHistory: new edit drops the redo stack", "[edithistory]")
{
	FGraphModel Model;
	FEditHistory History;
	Model.SetHistory(&History);

	const FNodeId IdA = Model.AddNode(std::make_shared<FGain>());
	const FNodeId IdB = Model.AddNode(std::make_shared<FGain>());
	(void)IdA;
	(void)IdB;
	History.Undo(Model);
	History.Undo(Model);
	REQUIRE(History.RedoSize() == 2);

	// New edit — redo stack should now be empty.
	Model.AddNode(std::make_shared<FOscillator>());
	REQUIRE(History.RedoSize() == 0);
}

TEST_CASE("EditHistory: capacity caps the undo stack at MaxEntries", "[edithistory]")
{
	FGraphModel Model;
	FEditHistory History;
	Model.SetHistory(&History);

	for (size_t I = 0; I < FEditHistory::MaxEntries + 50; ++I)
	{
		Model.AddNode(std::make_shared<FGain>());
	}
	REQUIRE(History.UndoSize() == FEditHistory::MaxEntries);
}

TEST_CASE("EditHistory: SetRecordHistory(false) suppresses recording", "[edithistory]")
{
	FGraphModel Model;
	FEditHistory History;
	Model.SetHistory(&History);

	Model.SetRecordHistory(false);
	Model.AddNode(std::make_shared<FGain>());
	REQUIRE(History.UndoSize() == 0);

	Model.SetRecordHistory(true);
	Model.AddNode(std::make_shared<FGain>());
	REQUIRE(History.UndoSize() == 1);
}

TEST_CASE("EditHistory: composite groups multiple edits into one undo unit", "[edithistory][composite]")
{
	FGraphModel Model;
	FEditHistory History;
	Model.SetHistory(&History);

	// Three node adds inside a composite → 1 undo entry, undoes all three at once.
	History.BeginComposite();
	const FNodeId A = Model.AddNode(std::make_shared<FGain>());
	const FNodeId B = Model.AddNode(std::make_shared<FGain>());
	const FNodeId C = Model.AddNode(std::make_shared<FGain>());
	History.EndComposite();
	(void)A; (void)B; (void)C;

	REQUIRE(Model.GetNodes().size() == 3);
	REQUIRE(History.UndoSize() == 1);

	REQUIRE(History.Undo(Model));
	REQUIRE(Model.GetNodes().size() == 0);
	REQUIRE(History.UndoSize() == 0);
	REQUIRE(History.RedoSize() == 1);

	REQUIRE(History.Redo(Model));
	REQUIRE(Model.GetNodes().size() == 3);
}

TEST_CASE("EditHistory: empty composite is a no-op", "[edithistory][composite]")
{
	FGraphModel Model;
	FEditHistory History;
	Model.SetHistory(&History);

	History.BeginComposite();
	History.EndComposite();
	REQUIRE(History.UndoSize() == 0);
}

TEST_CASE("EditHistory: composite undo unwinds in reverse so dependent commands undo first", "[edithistory][composite]")
{
	FGraphModel Model;
	FEditHistory History;
	Model.SetHistory(&History);

	History.BeginComposite();
	const FNodeId OscId = Model.AddNode(std::make_shared<FOscillator>());
	const FNodeId OutId = Model.AddNode(std::make_shared<FOutput>());
	Model.AddLink(OscId, 0, OutId, 0);
	History.EndComposite();

	REQUIRE(Model.GetNodes().size() == 2);
	REQUIRE(Model.GetLinks().size() == 1);

	// One undo unwinds: link removed first, then nodes — so the AddNode-undo
	// (which would otherwise also remove the link) finds nothing to do.
	REQUIRE(History.Undo(Model));
	REQUIRE(Model.GetNodes().size() == 0);
	REQUIRE(Model.GetLinks().size() == 0);
}

TEST_CASE("EditHistory: SetNodePosition round-trips through Undo + Redo", "[edithistory][position]")
{
	FGraphModel Model;
	FEditHistory History;
	Model.SetHistory(&History);

	const FNodeId Id = Model.AddNode(std::make_shared<FGain>(), 100.0f, 200.0f);
	History.Clear();

	NodeSynth::FEditCommand Cmd;
	Cmd.Type = NodeSynth::EEditCommand::SetNodePosition;
	Cmd.NodeId = Id;
	Cmd.OldX = 100.0f; Cmd.OldY = 200.0f;
	Cmd.NewX = 350.0f; Cmd.NewY = 80.0f;
	Model.GetNodes(); // settle reference (no-op)
	{
		auto* Rec = Model.FindNode(Id);
		Rec->PositionX = Cmd.NewX;
		Rec->PositionY = Cmd.NewY;
	}
	History.Push(Cmd);

	History.Undo(Model);
	REQUIRE(Model.GetNodes().at(Id).PositionX == 100.0f);
	REQUIRE(Model.GetNodes().at(Id).PositionY == 200.0f);

	History.Redo(Model);
	REQUIRE(Model.GetNodes().at(Id).PositionX == 350.0f);
	REQUIRE(Model.GetNodes().at(Id).PositionY == 80.0f);
}

TEST_CASE("EditHistory: GetUndoLabel describes the top-of-stack entry", "[edithistory][labels]")
{
	FGraphModel Model;
	FEditHistory History;
	Model.SetHistory(&History);

	Model.AddNode(std::make_shared<FOscillator>());
	const std::string Label = History.GetUndoLabel(0);
	REQUIRE(Label.find("Oscillator") != std::string::npos);

	History.BeginComposite();
	Model.AddNode(std::make_shared<FGain>());
	Model.AddNode(std::make_shared<FGain>());
	History.EndComposite();
	const std::string CompLabel = History.GetUndoLabel(0);
	REQUIRE(CompLabel.find("Batch") != std::string::npos);
}

TEST_CASE("EditHistory: Clear empties both stacks", "[edithistory]")
{
	FGraphModel Model;
	FEditHistory History;
	Model.SetHistory(&History);

	Model.AddNode(std::make_shared<FGain>());
	Model.AddNode(std::make_shared<FOscillator>());
	History.Undo(Model);
	REQUIRE(History.UndoSize() == 1);
	REQUIRE(History.RedoSize() == 1);

	History.Clear();
	REQUIRE(History.UndoSize() == 0);
	REQUIRE(History.RedoSize() == 0);
}
