#include "sid/PsidLoader.h"
#include "sid/SidEmulator.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using NodeSynth::ELoadError;
using NodeSynth::EPsidTimer;
using NodeSynth::FLoadedSidTune;
using NodeSynth::FSidEmulator;
using NodeSynth::FSidRegisterWrite;
using NodeSynth::ParseSidBytes;

namespace
{
	// Build a minimal PSID v2 header in-place. Field offsets per the HVSC
	// SID file format spec; all multi-byte values stored big-endian.
	std::vector<uint8_t> BuildPsidV2Header(
		uint16_t LoadAddr, uint16_t InitAddr, uint16_t PlayAddr,
		uint16_t Songs = 1, uint16_t StartSong = 1, uint32_t Speed = 0,
		uint16_t Flags = 0)
	{
		std::vector<uint8_t> H(0x7C, 0);
		H[0] = 'P'; H[1] = 'S'; H[2] = 'I'; H[3] = 'D';
		H[4] = 0;    H[5] = 2;                              // version
		H[6] = 0;    H[7] = 0x7C;                            // dataOffset
		H[8] = static_cast<uint8_t>(LoadAddr >> 8); H[9] = static_cast<uint8_t>(LoadAddr & 0xFF);
		H[10] = static_cast<uint8_t>(InitAddr >> 8); H[11] = static_cast<uint8_t>(InitAddr & 0xFF);
		H[12] = static_cast<uint8_t>(PlayAddr >> 8); H[13] = static_cast<uint8_t>(PlayAddr & 0xFF);
		H[14] = static_cast<uint8_t>(Songs >> 8);    H[15] = static_cast<uint8_t>(Songs & 0xFF);
		H[16] = static_cast<uint8_t>(StartSong >> 8); H[17] = static_cast<uint8_t>(StartSong & 0xFF);
		H[18] = static_cast<uint8_t>((Speed >> 24) & 0xFF);
		H[19] = static_cast<uint8_t>((Speed >> 16) & 0xFF);
		H[20] = static_cast<uint8_t>((Speed >> 8) & 0xFF);
		H[21] = static_cast<uint8_t>(Speed & 0xFF);
		H[0x76] = static_cast<uint8_t>(Flags >> 8);
		H[0x77] = static_cast<uint8_t>(Flags & 0xFF);
		// Name / Author / Released left blank for these tests.
		return H;
	}
}

TEST_CASE("PSID parser rejects bad magic", "[sid][psid]")
{
	std::vector<uint8_t> Buf(0x80, 0);
	Buf[0] = 'X'; Buf[1] = 'X'; Buf[2] = 'X'; Buf[3] = 'X';
	ELoadError Err;
	auto R = ParseSidBytes(Buf.data(), Buf.size(), Err);
	REQUIRE(!R.has_value());
	REQUIRE(Err == ELoadError::BadMagic);
}

TEST_CASE("PSID parser rejects RSID v3", "[sid][psid]")
{
	std::vector<uint8_t> Buf(0x80, 0);
	Buf[0] = 'R'; Buf[1] = 'S'; Buf[2] = 'I'; Buf[3] = 'D';
	Buf[4] = 0;   Buf[5] = 3;
	Buf[6] = 0;   Buf[7] = 0x7C;
	ELoadError Err;
	auto R = ParseSidBytes(Buf.data(), Buf.size(), Err);
	REQUIRE(!R.has_value());
	REQUIRE(Err == ELoadError::RsidUnsupported);
}

TEST_CASE("PSID parser rejects truncated header", "[sid][psid]")
{
	std::vector<uint8_t> Buf(0x40, 0);
	Buf[0] = 'P'; Buf[1] = 'S'; Buf[2] = 'I'; Buf[3] = 'D';
	ELoadError Err;
	auto R = ParseSidBytes(Buf.data(), Buf.size(), Err);
	REQUIRE(!R.has_value());
	REQUIRE(Err == ELoadError::FileTooShort);
}

TEST_CASE("PSID parser extracts header fields and bytecode", "[sid][psid]")
{
	std::vector<uint8_t> File = BuildPsidV2Header(
		/*LoadAddr*/ 0x1000,
		/*InitAddr*/ 0x1000,
		/*PlayAddr*/ 0x1003,
		/*Songs*/    3,
		/*StartSong*/ 2,
		/*Speed*/    0x00000002,  // bit 1 set → subtune 2 uses CIA mode
		/*Flags*/    0x14);       // bits 2-3 = 1 (PAL), bits 4-5 = 1 (6581)

	// Append a tiny payload after the header — the "bytecode" — RTS at $1000.
	File.push_back(0x60);  // RTS
	File.push_back(0xEA);  // NOP
	File.push_back(0xEA);

	ELoadError Err;
	auto R = ParseSidBytes(File.data(), File.size(), Err);
	REQUIRE(R.has_value());
	REQUIRE(Err == ELoadError::None);

	const FLoadedSidTune& T = *R;
	REQUIRE(T.Header.Version == 2);
	REQUIRE(T.LoadAddr == 0x1000);
	REQUIRE(T.Header.InitAddr == 0x1000);
	REQUIRE(T.Header.PlayAddr == 0x1003);
	REQUIRE(T.Header.Songs == 3);
	REQUIRE(T.Header.StartSong == 2);
	REQUIRE(T.Bytecode.size() == 3);
	REQUIRE(T.Bytecode[0] == 0x60);

	// Subtune 2 → bit 1 of Speed is set → CIA mode.
	REQUIRE(NodeSynth::GetTimerForSubtune(T.Header, 2) == EPsidTimer::Cia);
	// Subtune 1 → bit 0 not set → VBI.
	REQUIRE(NodeSynth::GetTimerForSubtune(T.Header, 1) == EPsidTimer::Vbi);
}

TEST_CASE("PSID parser handles embedded load address (.prg style)", "[sid][psid]")
{
	std::vector<uint8_t> File = BuildPsidV2Header(
		/*LoadAddr*/ 0x0000,  // 0 → embedded
		/*InitAddr*/ 0x4000,
		/*PlayAddr*/ 0x4003);
	// First two bytes of data segment carry the actual load address (LE).
	File.push_back(0x00);    // lo: $00
	File.push_back(0x40);    // hi: $40 → load at $4000
	File.push_back(0x60);    // RTS at $4000

	ELoadError Err;
	auto R = ParseSidBytes(File.data(), File.size(), Err);
	REQUIRE(R.has_value());
	REQUIRE(R->LoadAddr == 0x4000);
	REQUIRE(R->Bytecode.size() == 1);
	REQUIRE(R->Bytecode[0] == 0x60);
}

TEST_CASE("PSID init+play with CIA-mode timer fires play through CIA-1 IRQs", "[sid][psid][cia]")
{
	// CIA-mode synthetic tune. Init programs CIA-1 timer A to underflow at
	// ~50 Hz and enables timer A IRQ; the rest mirrors the VBI test. With a
	// correctly-wired CIA → M6502_IRQ path, play fires at the configured
	// rate exactly as VBI mode does.
	//
	// CIA-1 timer A latch period at PAL: 985248 / 50 ≈ 19704 = $4D08.
	// Layout:
	//   Init at $1000  (22 bytes total):
	//     LDA #$08; STA $DC04   ; timer A latch lo
	//     LDA #$4D; STA $DC05   ; timer A latch hi
	//     LDA #$81; STA $DC0D   ; ICR: enable timer A IRQ (bit 7 + bit 0)
	//     LDA #$11; STA $DC0E   ; CRA: start timer A, force load
	//     CLI
	//     RTS
	//   Play at $1016 (= $1000 + 22):
	//     LDA #$BC; STA $D400   ; voice 1 freq lo
	//     LDA #$DE; STA $D401   ; voice 1 freq hi
	//     LDA $DC0D             ; read CIA-1 ICR to ACK the timer A IRQ
	//     RTS
	const uint8_t Code[] = {
		// Init at $1000 (offset 0..21):
		0xA9, 0x08, 0x8D, 0x04, 0xDC,           // LDA #$08; STA $DC04
		0xA9, 0x4D, 0x8D, 0x05, 0xDC,           // LDA #$4D; STA $DC05
		0xA9, 0x81, 0x8D, 0x0D, 0xDC,           // LDA #$81; STA $DC0D
		0xA9, 0x11, 0x8D, 0x0E, 0xDC,           // LDA #$11; STA $DC0E
		0x58,                                    // CLI
		0x60,                                    // RTS
		// Play at $1016 (offset 22..35):
		0xA9, 0xBC, 0x8D, 0x00, 0xD4,           // LDA #$BC; STA $D400
		0xA9, 0xDE, 0x8D, 0x01, 0xD4,           // LDA #$DE; STA $D401
		0xAD, 0x0D, 0xDC,                       // LDA $DC0D (ack)
		0x60,                                    // RTS
	};
	std::vector<uint8_t> File = BuildPsidV2Header(
		/*LoadAddr*/ 0x1000,
		/*InitAddr*/ 0x1000,
		/*PlayAddr*/ 0x1016,
		/*Songs*/    1,
		/*StartSong*/ 1,
		/*Speed*/    1,    // bit 0 set → CIA mode for subtune 1
		/*Flags*/    0x14);
	File.insert(File.end(), Code, Code + sizeof(Code));

	ELoadError Err;
	auto R = ParseSidBytes(File.data(), File.size(), Err);
	REQUIRE(R.has_value());

	FSidEmulator Emu;
	const double ChipClock = 985248.0;
	Emu.Reset(ChipClock, 48000.0);
	REQUIRE(NodeSynth::LoadAndInit(Emu, *R, /*Subtune*/ 1, ChipClock, Err));

	std::vector<FSidRegisterWrite> AllWrites;
	const uint32_t SamplesToRun = static_cast<uint32_t>(48000.0 * 0.05);
	for (uint32_t S = 0; S < SamplesToRun; ++S)
	{
		Emu.TickOneAudioSample(AllWrites, 0);
	}

	bool bSawFreqLo = false;
	bool bSawFreqHi = false;
	for (const FSidRegisterWrite& W : AllWrites)
	{
		if (W.Reg == 0x00 && W.Value == 0xBC) { bSawFreqLo = true; }
		if (W.Reg == 0x01 && W.Value == 0xDE) { bSawFreqHi = true; }
	}
	REQUIRE(bSawFreqLo);
	REQUIRE(bSawFreqHi);
}

TEST_CASE("PSID init+play executes through the emulator", "[sid][psid]")
{
	// Synthetic tune layout, all at $1000:
	//   Init ($1000):
	//     LDA #$0F        A9 0F
	//     STA $D418       8D 18 D4   ; volume = max
	//     CLI             58         ; enable IRQs
	//     RTS             60
	//   Play ($1007):
	//     LDA #$34        A9 34
	//     STA $D400       8D 00 D4   ; voice 1 freq lo
	//     LDA #$12
	//     STA $D401       8D 01 D4   ; voice 1 freq hi
	//     RTS             60
	const uint8_t Code[] = {
		0xA9, 0x0F,
		0x8D, 0x18, 0xD4,
		0x58,
		0x60,
		0xA9, 0x34,
		0x8D, 0x00, 0xD4,
		0xA9, 0x12,
		0x8D, 0x01, 0xD4,
		0x60,
	};
	std::vector<uint8_t> File = BuildPsidV2Header(
		/*LoadAddr*/ 0x1000,
		/*InitAddr*/ 0x1000,
		/*PlayAddr*/ 0x1007,
		/*Songs*/    1,
		/*StartSong*/ 1,
		/*Speed*/    0,     // VBI
		/*Flags*/    0x14); // PAL/6581
	File.insert(File.end(), Code, Code + sizeof(Code));

	ELoadError Err;
	auto R = ParseSidBytes(File.data(), File.size(), Err);
	REQUIRE(R.has_value());

	FSidEmulator Emu;
	const double ChipClock = 985248.0;
	Emu.Reset(ChipClock, 48000.0);

	REQUIRE(NodeSynth::LoadAndInit(Emu, *R, /*Subtune*/ 1, ChipClock, Err));

	// Run the audio loop past one VBI tick (50 Hz = 20 ms). Play's STAs
	// to $D400/$D401 should appear in the captured writes. Init's
	// $D418 write happened during RunInitRoutine (no tap there) — that
	// path is verified by SidEmulator's standalone tests.
	std::vector<FSidRegisterWrite> AllWrites;
	const uint32_t SamplesToRun = static_cast<uint32_t>(48000.0 * 0.05); // 50 ms
	for (uint32_t S = 0; S < SamplesToRun; ++S)
	{
		Emu.TickOneAudioSample(AllWrites, 0);
	}

	bool bSawFreqLo = false;
	bool bSawFreqHi = false;
	for (const FSidRegisterWrite& W : AllWrites)
	{
		if (W.Reg == 0x00 && W.Value == 0x34) { bSawFreqLo = true; }
		if (W.Reg == 0x01 && W.Value == 0x12) { bSawFreqHi = true; }
	}
	REQUIRE(bSawFreqLo);
	REQUIRE(bSawFreqHi);
}
