#include "io/PresetBrowser.h"

#include <algorithm>
#include <map>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace NodeSynth
{
	namespace
	{
		// Pulls every .json file out of Dir into Out, keyed by category (the
		// immediate subdirectory of Dir). Files at Dir's root land under "".
		// Recursion is one level deep — flat-file or one-level subdirs only.
		// Anything deeper gets flattened into its top-level category, so a
		// stray nested folder doesn't disappear from the menu.
		void ScanRoot(
			const std::filesystem::path& Dir,
			std::map<std::string, std::vector<FPresetEntry>>& Out)
		{
			std::error_code Ec;
			if (!std::filesystem::exists(Dir, Ec) || !std::filesystem::is_directory(Dir, Ec))
			{
				return;
			}
			for (const auto& TopEntry : std::filesystem::directory_iterator(Dir, Ec))
			{
				if (Ec) { break; }
				if (TopEntry.is_directory(Ec))
				{
					const std::string Category = TopEntry.path().filename().string();
					for (const auto& Inner : std::filesystem::recursive_directory_iterator(TopEntry.path(), Ec))
					{
						if (Ec) { break; }
						if (Inner.is_regular_file(Ec) && Inner.path().extension() == ".json")
						{
							FPresetEntry E;
							E.DisplayName = Inner.path().stem().string();
							E.FullPath = Inner.path();
							Out[Category].push_back(std::move(E));
						}
					}
				}
				else if (TopEntry.is_regular_file(Ec)
					&& TopEntry.path().extension() == ".json")
				{
					FPresetEntry E;
					E.DisplayName = TopEntry.path().stem().string();
					E.FullPath = TopEntry.path();
					Out[std::string()].push_back(std::move(E));
				}
			}
		}
	}

	FPresetIndex BuildPresetIndex(
		const std::filesystem::path& BundledDir,
		const std::filesystem::path& UserDir)
	{
		// Map keyed by category name; user dir overlays after bundled, so a
		// user preset with the same relative path overrides a bundled one.
		std::map<std::string, std::vector<FPresetEntry>> Buckets;
		ScanRoot(BundledDir, Buckets);

		std::map<std::string, std::vector<FPresetEntry>> UserBuckets;
		ScanRoot(UserDir, UserBuckets);
		for (auto& [Cat, Entries] : UserBuckets)
		{
			auto& Target = Buckets[Cat];
			for (auto& UserEntry : Entries)
			{
				const auto It = std::find_if(Target.begin(), Target.end(),
					[&](const FPresetEntry& E) { return E.DisplayName == UserEntry.DisplayName; });
				if (It != Target.end())
				{
					*It = std::move(UserEntry);
				}
				else
				{
					Target.push_back(std::move(UserEntry));
				}
			}
		}

		// Sort entries within each category alphabetically.
		FPresetIndex Index;
		Index.Categories.reserve(Buckets.size());
		for (auto& [Cat, Entries] : Buckets)
		{
			std::sort(Entries.begin(), Entries.end(),
				[](const FPresetEntry& A, const FPresetEntry& B)
				{
					return A.DisplayName < B.DisplayName;
				});
			FPresetCategory C;
			C.Name = Cat;
			C.Entries = std::move(Entries);
			Index.Categories.push_back(std::move(C));
		}
		// std::map already iterates by key order, so Categories is sorted by name
		// with the empty-name root bucket first (empty string sorts before any
		// non-empty name) — that matches the desired display order.
		return Index;
	}

	std::filesystem::path GetExecutableDir()
	{
		std::error_code Ec;
#ifdef _WIN32
		wchar_t Buf[MAX_PATH];
		const DWORD N = GetModuleFileNameW(nullptr, Buf, MAX_PATH);
		if (N > 0 && N < MAX_PATH)
		{
			return std::filesystem::path(Buf).parent_path();
		}
#elif defined(__APPLE__)
		uint32_t Size = 0;
		_NSGetExecutablePath(nullptr, &Size);
		std::string Buf(Size, '\0');
		if (Size > 0 && _NSGetExecutablePath(Buf.data(), &Size) == 0)
		{
			return std::filesystem::canonical(std::filesystem::path(Buf), Ec).parent_path();
		}
#elif defined(__linux__)
		char Buf[4096];
		const ssize_t N = readlink("/proc/self/exe", Buf, sizeof(Buf) - 1);
		if (N > 0)
		{
			Buf[N] = '\0';
			return std::filesystem::path(Buf).parent_path();
		}
#endif
		return std::filesystem::current_path(Ec);
	}

	std::filesystem::path GetBundledPresetDir()
	{
		const std::filesystem::path ExeDir = GetExecutableDir();
#ifdef __APPLE__
		std::error_code Ec;
		const std::filesystem::path BundlePath = ExeDir / ".." / "Resources" / "presets";
		if (std::filesystem::exists(BundlePath, Ec))
		{
			return std::filesystem::canonical(BundlePath, Ec);
		}
#endif
		return ExeDir / "presets";
	}
}
