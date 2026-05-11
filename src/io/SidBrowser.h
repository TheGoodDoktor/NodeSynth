#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace NodeSynth
{
	// One scanned .sid file.
	struct FSidEntry
	{
		std::string DisplayName;             // file stem
		std::filesystem::path FullPath;      // absolute path on disk
		std::filesystem::path RelativePath;  // relative to the scan root
	};

	struct FSidCategory
	{
		std::string Name;                    // subdirectory (empty = root)
		std::vector<FSidEntry> Entries;
	};

	struct FSidLibrary
	{
		std::vector<FSidCategory> Categories;
		bool IsEmpty() const { return Categories.empty(); }
	};

	// Scan both directories; user entries with the same display name as a
	// bundled entry override the bundled one. Missing dirs are silently
	// ignored. Mirrors BuildWavetableLibrary / BuildPresetIndex.
	FSidLibrary BuildSidLibrary(
		const std::filesystem::path& BundledDir,
		const std::filesystem::path& UserDir);

	std::filesystem::path GetBundledSidDir();
	std::filesystem::path GetUserSidDir();

	// Resolve a stored File-param string (relative or absolute) into an
	// absolute path the SID loader can open. Returns empty if no candidate
	// exists. Lookup order: absolute-as-given -> user dir -> bundled dir.
	std::filesystem::path ResolveSidPath(const std::string& Stored);
}
