#include "io/SubgraphBrowser.h"

#include "io/PresetBrowser.h"  // GetExecutableDir()

#include <algorithm>
#include <cstdlib>
#include <map>
#include <system_error>
#include <utility>

namespace NodeSynth
{
	namespace
	{
		void ScanRoot(const std::filesystem::path& Dir, bool bBundled,
			std::map<std::string, FSubgraphAsset>& Out)
		{
			std::error_code Ec;
			if (!std::filesystem::exists(Dir, Ec) || !std::filesystem::is_directory(Dir, Ec))
			{
				return;
			}
			for (const auto& Entry : std::filesystem::directory_iterator(Dir, Ec))
			{
				if (Ec) { break; }
				if (Entry.is_regular_file(Ec) && Entry.path().extension() == ".nspg")
				{
					FSubgraphAsset A;
					A.DisplayName = Entry.path().stem().string();
					A.FullPath = Entry.path();
					A.bBundled = bBundled;
					Out[A.DisplayName] = std::move(A);  // user pass overwrites bundled
				}
			}
		}

		// Settings dir lookup duplicated from main.cpp / WavetableBrowser so the
		// io/ layer doesn't need a circular include into the app shell.
		std::filesystem::path GetSettingsDir()
		{
#ifdef _WIN32
			const char* HomeVar = "USERPROFILE";
#else
			const char* HomeVar = "HOME";
#endif
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
			const char* Home = std::getenv(HomeVar);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
			std::filesystem::path Dir = Home
				? std::filesystem::path(Home) / ".nodesynth"
				: std::filesystem::path(".");
			std::error_code Ec;
			std::filesystem::create_directories(Dir, Ec);
			return Dir;
		}
	}

	std::vector<FSubgraphAsset> BuildSubgraphLibrary(
		const std::filesystem::path& BundledDir,
		const std::filesystem::path& UserDir)
	{
		std::map<std::string, FSubgraphAsset> Assets;
		ScanRoot(BundledDir, /*bBundled*/ true, Assets);
		ScanRoot(UserDir, /*bBundled*/ false, Assets);  // user shadows bundled

		std::vector<FSubgraphAsset> Result;
		Result.reserve(Assets.size());
		for (auto& [Name, Asset] : Assets)
		{
			Result.push_back(std::move(Asset));
		}
		std::sort(Result.begin(), Result.end(),
			[](const FSubgraphAsset& A, const FSubgraphAsset& B)
			{
				return A.DisplayName < B.DisplayName;
			});
		return Result;
	}

	std::filesystem::path GetBundledSubgraphDir()
	{
		const std::filesystem::path ExeDir = GetExecutableDir();
#ifdef __APPLE__
		std::error_code Ec;
		const std::filesystem::path BundlePath = ExeDir / ".." / "Resources" / "subgraphs";
		if (std::filesystem::exists(BundlePath, Ec))
		{
			return std::filesystem::canonical(BundlePath, Ec);
		}
#endif
		return ExeDir / "subgraphs";
	}

	std::filesystem::path GetUserSubgraphDir()
	{
		return GetSettingsDir() / "subgraphs";
	}
}
