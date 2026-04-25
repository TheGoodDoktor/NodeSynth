#include "graph/AudioCommand.h"
#include "graph/Graph.h"
#include "dsp/Gain.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

using NodeSynth::FAudioCommand;
using NodeSynth::FAudioCommandRing;
using NodeSynth::FAudioGraph;
using NodeSynth::FGain;
using NodeSynth::FGraphModel;
using NodeSynth::FNodeId;
using NodeSynth::FOscillator;
using NodeSynth::FOutput;

TEST_CASE("FAudioCommandRing preserves FIFO order under push/pop", "[command][ring]")
{
	FAudioCommandRing Ring;
	for (uint32_t I = 0; I < 10; ++I)
	{
		REQUIRE(Ring.Push(FAudioCommand::MakeSetParam(42, I, static_cast<float>(I) * 0.1f)));
	}
	for (uint32_t I = 0; I < 10; ++I)
	{
		FAudioCommand Out;
		REQUIRE(Ring.Pop(Out));
		REQUIRE(Out.NodeId == 42);
		REQUIRE(Out.ParamIndex == I);
		REQUIRE(Out.Value == static_cast<float>(I) * 0.1f);
	}
	REQUIRE(Ring.IsEmpty());
}

TEST_CASE("FAudioCommandRing reports full at capacity-1", "[command][ring]")
{
	FAudioCommandRing Ring;
	size_t Pushed = 0;
	for (size_t I = 0; I < FAudioCommandRing::Capacity + 10; ++I)
	{
		if (Ring.Push(FAudioCommand::MakeSetParam(1, 0, 0.0f)))
		{
			++Pushed;
		}
	}
	REQUIRE(Pushed == FAudioCommandRing::Capacity - 1);
}

TEST_CASE("FAudioCommandRing SPSC under concurrent producer/consumer", "[command][ring]")
{
	FAudioCommandRing Ring;
	constexpr int TotalCommands = 10000;
	std::atomic<bool> bDone{ false };
	std::vector<uint32_t> Received;
	Received.reserve(TotalCommands);

	std::thread Producer([&]
	{
		int Sent = 0;
		while (Sent < TotalCommands)
		{
			if (Ring.Push(FAudioCommand::MakeSetParam(1, static_cast<uint32_t>(Sent), 0.0f)))
			{
				++Sent;
			}
		}
		bDone.store(true);
	});

	while (!bDone.load() || !Ring.IsEmpty())
	{
		FAudioCommand C;
		if (Ring.Pop(C))
		{
			Received.push_back(C.ParamIndex);
		}
	}
	Producer.join();

	REQUIRE(static_cast<int>(Received.size()) == TotalCommands);
	for (int I = 0; I < TotalCommands; ++I)
	{
		REQUIRE(Received[I] == static_cast<uint32_t>(I));
	}
}

TEST_CASE("FAudioGraph::DrainCommands routes SetParam to the right node", "[command][graph]")
{
	FGraphModel Model;
	const FNodeId GainId = Model.AddNode(std::make_shared<FGain>());
	const FNodeId OutId = Model.AddNode(std::make_shared<FOutput>());
	Model.AddLink(GainId, 0, OutId, 0);

	auto Snapshot = Model.Compile(48000.0);
	REQUIRE(Snapshot->NodeById.count(GainId) == 1);
	REQUIRE(Snapshot->NodeById.count(OutId) == 1);

	// Default Gain is 1.0; push a SetParam to change it to 0.25.
	FAudioCommandRing Ring;
	Ring.Push(FAudioCommand::MakeSetParam(GainId, FGain::Param_Gain, 0.25f));

	Snapshot->DrainCommands(Ring);

	auto* Gain = dynamic_cast<FGain*>(Snapshot->NodeById.at(GainId));
	REQUIRE(Gain != nullptr);
	REQUIRE(Gain->GetParamValue(FGain::Param_Gain) == 0.25f);
}

TEST_CASE("FAudioGraph::DrainCommands silently drops unknown node ids", "[command][graph]")
{
	FGraphModel Model;
	const FNodeId GainId = Model.AddNode(std::make_shared<FGain>());
	const FNodeId OutId = Model.AddNode(std::make_shared<FOutput>());
	Model.AddLink(GainId, 0, OutId, 0);

	auto Snapshot = Model.Compile(48000.0);

	FAudioCommandRing Ring;
	Ring.Push(FAudioCommand::MakeSetParam(9999, 0, 0.5f)); // node not in snapshot
	Ring.Push(FAudioCommand::MakeSetParam(GainId, FGain::Param_Gain, 0.5f));

	Snapshot->DrainCommands(Ring);

	auto* Gain = dynamic_cast<FGain*>(Snapshot->NodeById.at(GainId));
	REQUIRE(Gain != nullptr);
	REQUIRE(Gain->GetParamValue(FGain::Param_Gain) == 0.5f);
	REQUIRE(Ring.IsEmpty()); // unknown ids are popped, not requeued
}

TEST_CASE("FAudioGraph::DrainCommands omits unreachable nodes from the lookup", "[command][graph]")
{
	FGraphModel Model;
	const FNodeId ReachableId = Model.AddNode(std::make_shared<FGain>());
	const FNodeId OutId = Model.AddNode(std::make_shared<FOutput>());
	const FNodeId UnreachableId = Model.AddNode(std::make_shared<FOscillator>()); // not connected
	Model.AddLink(ReachableId, 0, OutId, 0);

	auto Snapshot = Model.Compile(48000.0);
	REQUIRE(Snapshot->NodeById.count(ReachableId) == 1);
	REQUIRE(Snapshot->NodeById.count(OutId) == 1);
	REQUIRE(Snapshot->NodeById.count(UnreachableId) == 0);
}
