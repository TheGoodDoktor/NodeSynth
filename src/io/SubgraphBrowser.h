#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace NodeSynth
{
	// Drag-drop payload id used between the Subgraph Library panel (source) and
	// the graph editor canvas (target). The payload carries the asset's
	// absolute path as a null-terminated string.
	inline constexpr const char* SubgraphAssetPayloadId = "NODESYNTH_SUBGRAPH_ASSET";

	// One scanned `.nspg` subgraph asset.
	struct FSubgraphAsset
	{
		std::string DisplayName;            // file stem (e.g. "StereoFilter")
		std::filesystem::path FullPath;     // absolute path to load
		bool bBundled = false;              // shipped next to the binary vs user dir
	};

	// Scan both directories for *.nspg files. A user asset with the same display
	// name as a bundled one shadows it (per-user override). Missing directories
	// are silently skipped. Result is sorted by display name.
	std::vector<FSubgraphAsset> BuildSubgraphLibrary(
		const std::filesystem::path& BundledDir,
		const std::filesystem::path& UserDir);

	// Default locations — <exe-dir>/subgraphs/ and ~/.nodesynth/subgraphs/.
	std::filesystem::path GetBundledSubgraphDir();
	std::filesystem::path GetUserSubgraphDir();
}
