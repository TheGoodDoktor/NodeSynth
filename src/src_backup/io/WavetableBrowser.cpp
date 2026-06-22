#include "io/WavetableBrowser.h"

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
		// Recursive scan: any *.wav under Dir is added. Files at Dir's
		// root land in the empty-name category; one-level subdirectory
		// gives the entry a category. Deeper trees flatten into their
		// top-level subdirectory so a stray nested folder doesn't get
		// lost. Same shape as PresetBrowser::ScanRoot.
		void ScanRoot(
			const std::filesystem::path& Dir,
			std::map<std::string, std::vector<FWavetableEntry>>& Out)
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
						if (Inner.is_regular_file(Ec) && Inner.path().extension() == ".wav")
						{
							FWavetableEntry E;
							E.DisplayName = Inner.path().stem().string();
							E.FullPath = Inner.path();
							E.RelativePath = std::filesystem::relative(Inner.path(), Dir, Ec);
							Out[Category].push_back(std::move(E));
						}
					}
				}
				else if (TopEntry.is_regular_file(Ec)
					&& TopEntry.path().extension() == ".wav")
				{
					FWavetableEntry E;
					E.DisplayName = TopEntry.path().stem().string();
					E.FullPath = TopEntry.path();
					E.RelativePath = TopEntry.path().filename();
					Out[std::string()].push_back(std::move(E));
				}
			}
		}

		// Settings dir lookup duplicated from main.cpp. Kept inline so the
		// io/ layer doesn't need a circular include into the app shell;
		// ~10 lines and rarely changes.
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

	FWavetableLibrary BuildWavetableLibrary(
		const std::filesystem::path& BundledDir,
		const std::filesystem::path& UserDir)
	{
		std::map<std::string, std::vector<FWavetableEntry>> Buckets;
		ScanRoot(BundledDir, Buckets);

		std::map<std::string, std::vector<FWavetableEntry>> UserBuckets;
		ScanRoot(UserDir, UserBuckets);
		for (auto& [Cat, Entries] : UserBuckets)
		{
			auto& Target = Buckets[Cat];
			for (auto& UserEntry : Entries)
			{
				const auto It = std::find_if(Target.begin(), Target.end(),
					[&](const FWavetableEntry& E) { return E.DisplayName == UserEntry.DisplayName; });
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

		FWavetableLibrary Lib;
		Lib.Categories.reserve(Buckets.size());
		for (auto& [Cat, Entries] : Buckets)
		{
			std::sort(Entries.begin(), Entries.end(),
				[](const FWavetableEntry& A, const FWavetableEntry& B)
				{
					return A.DisplayName < B.DisplayName;
				});
			FWavetableCategory C;
			C.Name = Cat;
			C.Entries = std::move(Entries);
			Lib.Categories.push_back(std::move(C));
		}
		return Lib;
	}

	std::filesystem::path GetBundledWavetableDir()
	{
		const std::filesystem::path ExeDir = GetExecutableDir();
#ifdef __APPLE__
		std::error_code Ec;
		const std::filesystem::path BundlePath = ExeDir / ".." / "Resources" / "wavetables";
		if (std::filesystem::exists(BundlePath, Ec))
		{
			return std::filesystem::canonical(BundlePath, Ec);
		}
#endif
		return ExeDir / "wavetables";
	}

	std::filesystem::path GetUserWavetableDir()
	{
		return GetSettingsDir() / "wavetables";
	}

	std::filesystem::path ResolveWavetablePath(const std::string& Stored)
	{
		if (Stored.empty())
		{
			return {};
		}
		const std::filesystem::path P(Stored);
		std::error_code Ec;
		if (P.is_absolute() && std::filesystem::exists(P, Ec))
		{
			return P;
		}
		// Relative path: try user dir first (so per-user override beats
		// the bundled copy), then fall back to bundled.
		const std::filesystem::path User = GetUserWavetableDir() / P;
		if (std::filesystem::exists(User, Ec))
		{
			return User;
		}
		const std::filesystem::path Bundled = GetBundledWavetableDir() / P;
		if (std::filesystem::exists(Bundled, Ec))
		{
			return Bundled;
		}
		return {};
	}
}
