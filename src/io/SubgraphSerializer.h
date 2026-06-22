#pragma once

#include <filesystem>
#include <optional>

#include <nlohmann/json.hpp>

#include "graph/SubgraphDefinition.h"

namespace NodeSynth
{
	// JSON translation for subgraph definitions. The serialised object holds
	// the declared signature (input_pins / output_pins) plus the internal
	// graph as the same "nodes" / "links" shapes a patch uses (see GraphJson).
	//
	// The object produced by SerializeSubgraphDefinition IS the on-disk `.nspg`
	// content AND the value stored under a patch's top-level "subgraphs" map,
	// keyed by definition name — the two are interchangeable by design, so a
	// definition can be lifted in or out of a patch verbatim. See
	// docs/PLAN-SUBGRAPHS.md §1.9.

	nlohmann::json SerializeSubgraphDefinition(const FSubgraphDefinition& Def);

	// Returns nullopt only if the object is structurally invalid (missing /
	// empty name). Unknown internal node types and params are skipped with a
	// stderr warning, mirroring the patch loader.
	std::optional<FSubgraphDefinition> DeserializeSubgraphDefinition(const nlohmann::json& Obj);

	// Standalone `.nspg` asset I/O. SaveSubgraph writes pretty-printed JSON;
	// LoadSubgraph returns nullopt on file-not-found or parse error.
	bool SaveSubgraph(const FSubgraphDefinition& Def, const std::filesystem::path& Path);
	std::optional<FSubgraphDefinition> LoadSubgraph(const std::filesystem::path& Path);
}
