#include "sid/PsidLoader.h"

#include <cstring>
#include <fstream>

namespace NodeSynth
{
	namespace
	{
		// PSID/RSID headers store all multi-byte integers big-endian.
		uint16_t ReadBE16(const uint8_t* P) { return (uint16_t(P[0]) << 8) | P[1]; }
		uint32_t ReadBE32(const uint8_t* P)
		{
			return (uint32_t(P[0]) << 24) | (uint32_t(P[1]) << 16)
				| (uint32_t(P[2]) << 8) | P[3];
		}

		// Trim trailing NUL bytes from a fixed-size string field.
		std::string TrimAscii(const uint8_t* P, size_t Size)
		{
			size_t Len = 0;
			while (Len < Size && P[Len] != 0) { ++Len; }
			return std::string(reinterpret_cast<const char*>(P), Len);
		}
	}

	std::optional<FLoadedSidTune> ParseSidBytes(const uint8_t* Data, size_t Size, ELoadError& OutError)
	{
		OutError = ELoadError::None;

		// Header is at minimum 0x76 bytes (v1). v2/v3 add a few extra fields.
		if (Size < 0x76)
		{
			OutError = ELoadError::FileTooShort;
			return std::nullopt;
		}

		// Magic: "PSID" or "RSID". RSID v1/v2 doesn't exist (RSID is v3+),
		// but we accept the magic if present and reject specifically on
		// version, so the error message is more informative.
		const bool bPsid = std::memcmp(Data, "PSID", 4) == 0;
		const bool bRsid = std::memcmp(Data, "RSID", 4) == 0;
		if (!bPsid && !bRsid)
		{
			OutError = ELoadError::BadMagic;
			return std::nullopt;
		}

		FPsidHeader Header;
		Header.Version = ReadBE16(Data + 0x04);
		Header.DataOffset = ReadBE16(Data + 0x06);
		Header.LoadAddr = ReadBE16(Data + 0x08);
		Header.InitAddr = ReadBE16(Data + 0x0A);
		Header.PlayAddr = ReadBE16(Data + 0x0C);
		Header.Songs = ReadBE16(Data + 0x0E);
		Header.StartSong = ReadBE16(Data + 0x10);
		Header.Speed = ReadBE32(Data + 0x12);
		Header.Name = TrimAscii(Data + 0x16, 32);
		Header.Author = TrimAscii(Data + 0x36, 32);
		Header.Released = TrimAscii(Data + 0x56, 32);

		// v1 only goes up to 0x76; flags + extended fields are v2+.
		if (Header.Version >= 2 && Size >= 0x7C)
		{
			Header.Flags = ReadBE16(Data + 0x76);
		}

		if (bRsid || Header.Version >= 3)
		{
			OutError = ELoadError::RsidUnsupported;
			return std::nullopt;
		}
		if (Header.Version != 1 && Header.Version != 2)
		{
			OutError = ELoadError::UnsupportedVersion;
			return std::nullopt;
		}

		// Multi-SID tunes set bits in Flags (v3+ extension, but a few PSID v2
		// tunes use it). Bits 6-7 carry the second-SID address; bits 8-9 the
		// third-SID address. v1 doesn't support multi-SID; reject.
		if (Header.Version >= 2 && (Header.Flags & 0x03C0) != 0)
		{
			OutError = ELoadError::MultiSidUnsupported;
			return std::nullopt;
		}

		// Data segment starts at DataOffset (0x76 v1, 0x7C v2). Some files
		// lie about the offset; clamp to the spec values to be robust.
		const size_t DataOffset = (Header.Version == 1) ? 0x76 : 0x7C;
		if (Size <= DataOffset)
		{
			OutError = ELoadError::MalformedDataSegment;
			return std::nullopt;
		}

		FLoadedSidTune Tune;
		Tune.Header = Header;

		// LoadAddr == 0 means the first 2 bytes of the data segment carry
		// the actual load address, .prg-style (little-endian).
		const uint8_t* DataSeg = Data + DataOffset;
		const size_t DataLen = Size - DataOffset;
		if (Header.LoadAddr == 0)
		{
			if (DataLen < 2)
			{
				OutError = ELoadError::MalformedDataSegment;
				return std::nullopt;
			}
			Tune.LoadAddr = static_cast<uint16_t>(DataSeg[0]) | (static_cast<uint16_t>(DataSeg[1]) << 8);
			Tune.Bytecode.assign(DataSeg + 2, DataSeg + DataLen);
		}
		else
		{
			Tune.LoadAddr = Header.LoadAddr;
			Tune.Bytecode.assign(DataSeg, DataSeg + DataLen);
		}

		return Tune;
	}

	std::optional<FLoadedSidTune> LoadSidFile(const std::filesystem::path& Path, ELoadError& OutError)
	{
		std::ifstream In(Path, std::ios::binary);
		if (!In)
		{
			OutError = ELoadError::FileNotFound;
			return std::nullopt;
		}
		// Slurp.
		std::vector<uint8_t> Buf((std::istreambuf_iterator<char>(In)),
			std::istreambuf_iterator<char>());
		return ParseSidBytes(Buf.data(), Buf.size(), OutError);
	}

	EPsidTimer GetTimerForSubtune(const FPsidHeader& Header, uint16_t Subtune)
	{
		// PSID Speed is a 32-bit bitmap with bit N corresponding to subtune
		// N+1. Subtunes beyond bit 31 read bit 31. Bit 0 = VBI, bit 1 = CIA.
		const uint16_t Idx = (Subtune == 0) ? 0 : (Subtune - 1);
		const uint16_t BitPos = (Idx > 31) ? 31 : Idx;
		const bool bCia = (Header.Speed & (1u << BitPos)) != 0;
		return bCia ? EPsidTimer::Cia : EPsidTimer::Vbi;
	}

	bool IsNtscFromFlags(uint16_t Flags)
	{
		// Bits 2-3: 0=unknown/default(PAL), 1=PAL, 2=NTSC, 3=both.
		const uint16_t Region = (Flags >> 2) & 0x3;
		return Region == 2;  // strict NTSC only
	}

	bool Is8580FromFlags(uint16_t Flags)
	{
		// Bits 4-5: 0=unknown, 1=6581, 2=8580, 3=both.
		const uint16_t Model = (Flags >> 4) & 0x3;
		return Model == 2;
	}

	bool LoadAndInit(FSidEmulator& Emu, const FLoadedSidTune& Tune,
		uint16_t Subtune, double ChipClockHz, ELoadError& OutError)
	{
		OutError = ELoadError::None;

		// 1) Load bytecode into RAM at the resolved load address.
		Emu.LoadIntoRam(Tune.Bytecode.data(), Tune.Bytecode.size(), Tune.LoadAddr);

		// 2) Boot the CPU at the init address. PSID convention is that init
		//    runs once with A = subtune index (0-based — most tunes use it
		//    that way; some ignore A and read the song from an internal
		//    variable). We boot to InitAddr and let RunInitRoutine push a
		//    sentinel so we know when init returns.
		Emu.BootCpu(Tune.Header.InitAddr);

		// 3) Run init. Subtune is 1-based on the PSID spec; convert to 0-based
		//    for the A register convention.
		const uint8_t SubtuneIdx = (Subtune == 0) ? 0 : static_cast<uint8_t>(Subtune - 1);
		if (!Emu.RunInitRoutine(Tune.Header.InitAddr, SubtuneIdx))
		{
			OutError = ELoadError::MalformedDataSegment;  // init didn't return
			return false;
		}

		// 4) Install the IRQ stub pointing at PlayAddr.
		Emu.InstallPlayHook(Tune.Header.PlayAddr);

		// 5) Configure the VBI timer per the speed bitmap. CIA mode tunes
		//    leave the virtual VBI timer disabled and rely on the m6526 the
		//    init routine already programmed.
		const EPsidTimer Mode = GetTimerForSubtune(Tune.Header, Subtune);
		const bool bNtsc = IsNtscFromFlags(Tune.Header.Flags);
		const double VbiHz = bNtsc ? 60.0 : 50.0;
		Emu.SetVbiTimer(Mode == EPsidTimer::Vbi, VbiHz);
		(void)ChipClockHz;  // already baked into emulator state via Reset

		return true;
	}
}
