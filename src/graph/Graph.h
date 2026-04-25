#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "dsp/Node.h"
#include "graph/AudioCommand.h"

namespace NodeSynth
{
	struct FNodeRecord
	{
		FNodeId Id = 0;
		std::shared_ptr<INode> Node;
		float PositionX = 0.0f;
		float PositionY = 0.0f;
	};

	struct FLink
	{
		FLinkId Id = 0;
		FNodeId FromNode = 0;
		uint32_t FromPort = 0;
		FNodeId ToNode = 0;
		uint32_t ToPort = 0;
	};

	// Audio-thread-side graph snapshot. Immutable after construction. Nodes are
	// shared with the UI thread, but ownership is conceptually joint: both
	// threads hold the shared_ptr so neither destroys on the audio thread.
	class FAudioGraph
	{
	public:
		std::vector<std::shared_ptr<INode>> OrderedNodes;  // topological order (producers first)
		std::shared_ptr<INode> OutputNode;                 // null until the user adds one

		// Id → node lookup populated at compile time. Used by DrainCommands to
		// route incoming SetParam (and future) commands to the right node.
		// Held by raw INode* — the shared_ptrs in OrderedNodes keep the nodes
		// alive for the lifetime of this snapshot.
		std::unordered_map<FNodeId, INode*> NodeById;

		// Drains all pending commands from the ring and dispatches them. RT-safe:
		// the ring is lock-free and SetParamValue stores into atomics. Commands
		// targeting nodes not in this snapshot are silently dropped (the
		// equivalent UI-thread atomic write keeps the model consistent).
		void DrainCommands(FAudioCommandRing& Ring);

		void Process(const FProcessContext& Ctx)
		{
			for (auto& Node : OrderedNodes)
			{
				Node->Process(Ctx);
			}
		}
	};

	// UI-thread-side editable graph. Every structural edit (AddNode, AddLink, ...)
	// invalidates any previously compiled FAudioGraph; the caller should
	// Compile() and publish the new snapshot to the audio thread.
	class FGraphModel
	{
	public:
		FNodeId AddNode(std::shared_ptr<INode> Node, float X = 0.0f, float Y = 0.0f);

		// Restores a node with a specific id. Used by patch load to preserve the
		// ids stored in the file so links resolve correctly. Bumps NextNodeId so
		// future AddNode calls don't collide. Returns 0 if the id is already in
		// use, or if the node is a duplicate FOutput (singleton enforcement).
		FNodeId AddNodeWithId(FNodeId Id, std::shared_ptr<INode> Node, float X = 0.0f, float Y = 0.0f);

		void RemoveNode(FNodeId Id);

		// Returns 0 on failure (missing node, port out of range, type mismatch, cycle).
		// Replaces any existing link terminating at the same input port.
		FLinkId AddLink(FNodeId FromNode, uint32_t FromPort, FNodeId ToNode, uint32_t ToPort);
		void RemoveLink(FLinkId Id);

		// Builds an immutable audio-graph snapshot. Also writes input-buffer
		// pointers into each node, so the snapshot is ready to Process().
		std::shared_ptr<FAudioGraph> Compile(double SampleRate);

		const std::unordered_map<FNodeId, FNodeRecord>& GetNodes() const { return Nodes; }
		const std::vector<FLink>& GetLinks() const { return Links; }
		FNodeRecord* FindNode(FNodeId Id);

	private:
		std::unordered_map<FNodeId, FNodeRecord> Nodes;
		std::vector<FLink> Links;
		FNodeId NextNodeId = 1;
		FLinkId NextLinkId = 1;
	};
}
