#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "graph/AudioCommand.h"
#include "graph/Graph.h"

namespace NodeSynth::GraphJson
{
	// Per-node / per-link JSON translation shared by the patch serializer and
	// the subgraph serializer. Both a patch and a subgraph definition store
	// their graph as the same "nodes" + "links" array shapes, so the encoding
	// lives here once. See docs/PLAN-SUBGRAPHS.md §SG.1.

	// Serialises a single node record: id, type, position, optional per_voice
	// flag, and a name-keyed params object (string params via GetParamString,
	// everything else via GetParamValue).
	nlohmann::json SerializeNode(const FNodeRecord& Rec);

	// Serialises a single link: id + endpoints.
	nlohmann::json SerializeLink(const FLink& L);

	// Loads a "nodes" array into Model via AddNodeWithId, restoring per-voice
	// flags and params. Unknown / deprecated node types and unknown params are
	// skipped with a stderr warning; valid content still loads. When
	// OutInitialParams is non-null, a SetParam command is appended for every
	// numeric param so the caller can replay them onto the audio command ring.
	void DeserializeNodes(const nlohmann::json& NodesArray, FGraphModel& Model,
		std::vector<FAudioCommand>* OutInitialParams);

	// Loads a "links" array into Model via AddLink. Rejected links are skipped
	// with a stderr warning.
	void DeserializeLinks(const nlohmann::json& LinksArray, FGraphModel& Model);

	// Binds every FSubgraph instance in Model to its definition. Walks
	// NodesArray; for each node carrying a "subgraph_name", finds the matching
	// node in Model and points it at Defs[name]. Run after both the definitions
	// map and the nodes are loaded. Names with no matching definition are left
	// unbound (the instance keeps zero ports).
	void BindSubgraphInstances(FGraphModel& Model, const nlohmann::json& NodesArray,
		const std::unordered_map<std::string, std::shared_ptr<FSubgraphDefinition>>& Defs);
}
