#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "dsp/Node.h"
#include "graph/AudioCommand.h"

namespace NodeSynth
{
	class FEditHistory;
	class FVoiceAllocator;

	struct FNodeRecord
	{
		FNodeId Id = 0;
		std::shared_ptr<INode> Node;
		float PositionX = 0.0f;
		float PositionY = 0.0f;
		// When true, the graph compiler clones this node NumVoices times
		// during Compile() and wires the clones in parallel. Only meaningful
		// for nodes whose Clone() returns non-null. Set via FGraphModel::SetNodePerVoice.
		bool bPerVoice = false;
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

		// Id → node entry populated at compile time. Primary is the model's
		// original node (the one with the user's atomic param state); Voices
		// holds per-voice clones so SetParam can fan out to every clone of a
		// per-voice node. Empty Voices for global nodes.
		struct FNodeEntry
		{
			INode* Primary = nullptr;
			std::vector<INode*> Voices;
		};
		std::unordered_map<FNodeId, FNodeEntry> NodeById;

		// Voice allocators in this snapshot. NoteOn / NoteOff commands are
		// broadcast to every entry. Populated alongside NodeById in Compile.
		std::vector<FVoiceAllocator*> Allocators;

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

		// Restores a link with a specific id (used by undo/redo replay).
		// Returns 0 if the id is already in use or validation fails.
		FLinkId AddLinkWithId(FLinkId Id, FNodeId FromNode, uint32_t FromPort, FNodeId ToNode, uint32_t ToPort);

		void RemoveLink(FLinkId Id);

		// Edit history hook. When set and recording is enabled, every mutator
		// pushes an FEditCommand onto the history. Undo/Redo replay temporarily
		// disables recording so the playback doesn't recurse.
		void SetHistory(FEditHistory* InHistory) { History = InHistory; }
		void SetRecordHistory(bool b) { bRecordHistory = b; }
		bool IsRecordingHistory() const { return bRecordHistory && History != nullptr; }
		FEditHistory* GetHistory() const { return History; }

		// Builds an immutable audio-graph snapshot. Also writes input-buffer
		// pointers into each node, so the snapshot is ready to Process().
		std::shared_ptr<FAudioGraph> Compile(double SampleRate);

		const std::unordered_map<FNodeId, FNodeRecord>& GetNodes() const { return Nodes; }
		const std::vector<FLink>& GetLinks() const { return Links; }
		FNodeRecord* FindNode(FNodeId Id);

		// Toggles the per-voice flag on a node. Returns false (and leaves the
		// flag untouched) if the node id is unknown or the node's Clone() is
		// nullptr (non-cloneable types like MIDI input or virtual keyboard).
		bool SetNodePerVoice(FNodeId Id, bool bPerVoice);

	private:
		std::unordered_map<FNodeId, FNodeRecord> Nodes;
		std::vector<FLink> Links;
		FNodeId NextNodeId = 1;
		FLinkId NextLinkId = 1;
		FEditHistory* History = nullptr;
		bool bRecordHistory = true;
	};
}
