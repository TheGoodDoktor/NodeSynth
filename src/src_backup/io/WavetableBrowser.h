#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace NodeSynth
{
	// One scanned wavetable file.
	struct FWavetableEntry
	{
		// File stem (e.g. "FMBell"), used as the dropdown label.
		std::string DisplayName;
		// Absolute path. Passed to LoadWavetable when the user picks the entry.
		std::filesystem::path FullPath;
		// Path relative to the scan root, e.g. "FMBell.wav" or
		// "Vowels/AhEh.wav". Stored in patch JSON so presets remain
		// portable across machines (the bundled wavetables/ dir resolves
		// the same on every install).
		std::filesystem::path RelativePath;
	};

	struct FWavetableCategory
	{
		// Subdirectory name. Empty for entries directly in the scan root.
		std::string Name;
		std::vector<FWavetableEntry> Entries;
	};

	struct FWavetableLibrary
	{
		std::vector<FWavetableCategory> Categories;
		bool IsEmpty() const { return Categories.empty(); }
	};

	// Scan both directories for *.wav files. User entries with the same
	// relative path as a bundled entry shadow the bundled one (per-user
	// override). Missing directories are silently skipped. Same scan
	// shape as BuildPresetIndex.
	FWavetableLibrary BuildWavetableLibrary(
		const std::filesystem::path& BundledDir,
		const std::filesystem::path& UserDir);

	// Default locations — <exe-dir>/wavetables/ and ~/.nodesynth/wavetables/.
	std::filesystem::path GetBundledWavetableDir();
	std::filesystem::path GetUserWavetableDir();

	// Resolve a stored Wavetable param string (which may be a relative
	// path like "FMBell.wav" or an absolute path) into an absolute path
	// that LoadWavetable can open. Returns the empty path if no candidate
	// exists. Lookup order: stored-path-as-absolute → user dir → bundled
	// dir.
	std::filesystem::path ResolveWavetablePath(const std::string& Stored);
}
