#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "graph/AudioCommand.h"
#include "graph/Graph.h"

namespace NodeSynth
{
	// Schema version. Bump on breaking changes. Loader rejects mismatched versions.
	inline constexpr int32_t PatchSchemaVersion = 1;

	struct FLoadedPatch
	{
		FGraphModel Model;
		// SetParam commands that mirror the loaded param values. Push these onto
		// the audio command ring after publishing the new compiled snapshot so
		// the audio thread also sees the loaded state, in queue order with
		// subsequent edits.
		std::vector<FAudioCommand> InitialParams;
	};

	// Writes the model out as JSON. Returns true on success. Any IO or
	// serialisation error is logged to stderr; the file may be partially written
	// if the OS interrupted the write — caller is responsible for atomic-rename
	// semantics if needed.
	bool SavePatch(const FGraphModel& Model, const std::filesystem::path& Path);

	// Reads a patch file. Returns nullopt on file-not-found, parse error, or
	// version mismatch. Unknown node types and unknown param names are skipped
	// with a warning; other valid content still loads.
	std::optional<FLoadedPatch> LoadPatch(const std::filesystem::path& Path);
}
