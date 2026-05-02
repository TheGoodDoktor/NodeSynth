#include "dsp/Gain.h"
#include "dsp/Output.h"
#include "graph/EditHistory.h"
#include "graph/Graph.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace NodeSynth;

namespace
{
	FNodeId AddGain(FGraphModel& M)
	{
		return M.AddNode(std::make_shared<FGain>());
	}

	FMidiMapping Mapping(uint8_t Channel, uint8_t Cc, FNodeId NodeId, uint32_t ParamIndex)
	{
		FMidiMapping M;
		M.Channel = Channel;
		M.Cc = Cc;
		M.NodeId = NodeId;
		M.ParamIndex = ParamIndex;
		return M;
	}
}

TEST_CASE("FGraphModel: AddMidiMapping appends to the mapping list", "[midi][mapping]")
{
	FGraphModel M;
	FNodeId N = AddGain(M);
	M.AddMidiMapping(Mapping(1, 16, N, 0));
	REQUIRE(M.GetMidiMappings().size() == 1);
	REQUIRE(M.GetMidiMappings()[0].Channel == 1);
	REQUIRE(M.GetMidiMappings()[0].Cc == 16);
	REQUIRE(M.GetMidiMappings()[0].NodeId == N);
}

TEST_CASE("FGraphModel: AddMidiMapping on an existing (Channel,Cc) replaces the prior one", "[midi][mapping]")
{
	FGraphModel M;
	FNodeId N1 = AddGain(M);
	FNodeId N2 = AddGain(M);
	M.AddMidiMapping(Mapping(1, 16, N1, 0));
	M.AddMidiMapping(Mapping(1, 16, N2, 0));    // same channel + CC, different target
	REQUIRE(M.GetMidiMappings().size() == 1);
	REQUIRE(M.GetMidiMappings()[0].NodeId == N2);
}

TEST_CASE("FGraphModel: RemoveMidiMapping removes the entry", "[midi][mapping]")
{
	FGraphModel M;
	FNodeId N = AddGain(M);
	M.AddMidiMapping(Mapping(1, 16, N, 0));
	M.AddMidiMapping(Mapping(1, 17, N, 0));
	M.RemoveMidiMapping(1, 16);
	REQUIRE(M.GetMidiMappings().size() == 1);
	REQUIRE(M.GetMidiMappings()[0].Cc == 17);
}

TEST_CASE("FGraphModel: RemoveNode sweeps mappings whose target was the removed node", "[midi][mapping]")
{
	FGraphModel M;
	FNodeId N1 = AddGain(M);
	FNodeId N2 = AddGain(M);
	M.AddMidiMapping(Mapping(1, 16, N1, 0));
	M.AddMidiMapping(Mapping(1, 17, N2, 0));
	M.RemoveNode(N1);
	REQUIRE(M.GetMidiMappings().size() == 1);
	REQUIRE(M.GetMidiMappings()[0].NodeId == N2);
}

TEST_CASE("FGraphModel: AddMidiMapping is undoable", "[midi][mapping][history]")
{
	FGraphModel M;
	FEditHistory H;
	M.SetHistory(&H);
	FNodeId N = AddGain(M);

	M.AddMidiMapping(Mapping(1, 16, N, 0));
	REQUIRE(M.GetMidiMappings().size() == 1);

	REQUIRE(H.Undo(M));
	REQUIRE(M.GetMidiMappings().empty());

	REQUIRE(H.Redo(M));
	REQUIRE(M.GetMidiMappings().size() == 1);
	REQUIRE(M.GetMidiMappings()[0].Cc == 16);
}

TEST_CASE("FGraphModel: AddMidiMapping that replaces another roundtrips through undo/redo", "[midi][mapping][history]")
{
	FGraphModel M;
	FEditHistory H;
	M.SetHistory(&H);
	FNodeId N1 = AddGain(M);
	FNodeId N2 = AddGain(M);

	M.AddMidiMapping(Mapping(1, 16, N1, 0));
	M.AddMidiMapping(Mapping(1, 16, N2, 0));    // replace
	REQUIRE(M.GetMidiMappings()[0].NodeId == N2);

	// Undo the replacement: should restore the original mapping pointing at N1.
	REQUIRE(H.Undo(M));
	REQUIRE(M.GetMidiMappings().size() == 1);
	REQUIRE(M.GetMidiMappings()[0].NodeId == N1);

	// Redo: back to N2.
	REQUIRE(H.Redo(M));
	REQUIRE(M.GetMidiMappings()[0].NodeId == N2);
}

TEST_CASE("FGraphModel: RemoveNode-undo restores the mappings that were swept", "[midi][mapping][history]")
{
	FGraphModel M;
	FEditHistory H;
	M.SetHistory(&H);
	FNodeId N = AddGain(M);
	M.AddMidiMapping(Mapping(1, 16, N, 0));
	M.AddMidiMapping(Mapping(2, 32, N, 0));
	REQUIRE(M.GetMidiMappings().size() == 2);

	M.RemoveNode(N);
	REQUIRE(M.GetNodes().count(N) == 0);
	REQUIRE(M.GetMidiMappings().empty());

	REQUIRE(H.Undo(M));
	REQUIRE(M.GetNodes().count(N) == 1);
	REQUIRE(M.GetMidiMappings().size() == 2);
}
