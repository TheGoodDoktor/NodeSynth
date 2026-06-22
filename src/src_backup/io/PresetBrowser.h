#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace NodeSynth
{
	struct FPresetEntry
	{
		// Display name shown in the menu — the .json filename without extension.
		std::string DisplayName;
		// Absolute path on disk; passed to LoadPatch when the user clicks.
		std::filesystem::path FullPath;
	};

	struct FPresetCategory
	{
		// Subdirectory name (e.g. "Bass", "Lead"). Empty for presets sitting
		// directly under the root preset dir.
		std::string Name;
		std::vector<FPresetEntry> Entries;
	};

	struct FPresetIndex
	{
		std::vector<FPresetCategory> Categories;
		bool IsEmpty() const { return Categories.empty(); }
	};

	// Scans the bundled preset directory and the user preset directory and
	// returns a merged, sorted index. User presets shadow bundled presets that
	// share the same relative path. Missing directories are silently ignored.
	FPresetIndex BuildPresetIndex(
		const std::filesystem::path& BundledDir,
		const std::filesystem::path& UserDir);

	// Directory containing the running binary. macOS .app: .../Contents/MacOS/.
	// Falls back to current_path() if the platform query fails.
	std::filesystem::path GetExecutableDir();

	// Resolves the bundled preset directory shipped next to the binary.
	// Windows / Linux: <exe-dir>/presets/.
	// macOS .app: <exe-dir>/../Resources/presets/ if it exists, with the
	// next-to-exe path as a dev-build fallback. Returns a path that may not
	// exist on disk — the caller's scan is a no-op in that case.
	std::filesystem::path GetBundledPresetDir();
}
