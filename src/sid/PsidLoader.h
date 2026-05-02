#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "sid/SidEmulator.h"

namespace NodeSynth
{
	// PSID v1/v2 header fields, parsed from the file's first 0x76 (v1) or
	// 0x7C (v2) bytes. RSID v3 is rejected at parse time with ELoadError::RsidUnsupported.
	struct FPsidHeader
	{
		uint16_t Version = 0;        // 1 or 2
		uint16_t DataOffset = 0;     // typically 0x76 (v1) or 0x7C (v2)
		uint16_t LoadAddr = 0;       // 0 means "first 2 bytes of data are load address"
		uint16_t InitAddr = 0;
		uint16_t PlayAddr = 0;
		uint16_t Songs = 0;
		uint16_t StartSong = 0;      // 1-based per PSID spec
		uint32_t Speed = 0;          // big-endian bitmap: bit N = subtune N+1; 0=VBI 1=CIA
		std::string Name;            // up to 32 chars, null-trimmed ASCII
		std::string Author;
		std::string Released;
		uint16_t Flags = 0;          // v2+; bits encode region (PAL/NTSC) + model (6581/8580)
	};

	enum class ELoadError
	{
		None,
		FileNotFound,
		FileTooShort,
		BadMagic,
		UnsupportedVersion,
		RsidUnsupported,
		MultiSidUnsupported,
		MalformedDataSegment,
	};

	// Final loaded result — header plus the bytecode ready to copy into RAM.
	// Bytecode + LoadAddr are normalised: if the file used the embedded-load-
	// address convention (header.LoadAddr == 0, first 2 bytes of the data
	// segment carry the address), the loader unwraps that here so the caller
	// always sees a plain (LoadAddr, Bytecode) pair.
	struct FLoadedSidTune
	{
		FPsidHeader Header;
		std::vector<uint8_t> Bytecode;
		uint16_t LoadAddr = 0;
	};

	// Parse a .sid file from disk. Returns the loaded tune on success, or
	// std::nullopt on failure with OutError populated. OutError must be
	// non-null.
	std::optional<FLoadedSidTune> LoadSidFile(
		const std::filesystem::path& Path, ELoadError& OutError);

	// Same as LoadSidFile but takes raw bytes instead of a file path.
	// Useful for tests that inline a synthetic PSID header without touching
	// the filesystem.
	std::optional<FLoadedSidTune> ParseSidBytes(
		const uint8_t* Data, size_t Size, ELoadError& OutError);

	// Returns the PSID timer mode for the given (1-based) subtune. Reads the
	// header.Speed bitmap: bit (Subtune-1) == 0 means VBI, == 1 means CIA.
	// Subtunes beyond bit 31 wrap to bit 31 per the PSID spec.
	EPsidTimer GetTimerForSubtune(const FPsidHeader& Header, uint16_t Subtune);

	// Region inferred from header.Flags (PAL=0, NTSC=1, both=2, unknown=3).
	// Bits 2-3 of Flags. Returns true=NTSC if explicitly NTSC, false=PAL
	// otherwise (PAL is the safe default for ambiguous tunes).
	bool IsNtscFromFlags(uint16_t Flags);

	// SID model inferred from header.Flags (bits 4-5: 0=unknown, 1=6581,
	// 2=8580, 3=both). Returns true if the tune was authored for 8580.
	bool Is8580FromFlags(uint16_t Flags);

	// One-shot convenience: parse, copy bytecode into the emulator's RAM at
	// the right load address, set up the IRQ stub for PlayAddr, configure the
	// VBI timer if needed, and run the init routine for the chosen subtune.
	// Returns true on full success. On failure, OutError is populated and the
	// emulator may be in a partial state — caller should Reset() before reuse.
	// Subtune is 1-based to match the PSID spec.
	bool LoadAndInit(FSidEmulator& Emu, const FLoadedSidTune& Tune,
		uint16_t Subtune, double ChipClockHz, ELoadError& OutError);
}
