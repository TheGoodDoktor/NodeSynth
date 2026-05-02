#include "sid/SidEmulator.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using NodeSynth::FSidEmulator;
using NodeSynth::FSidRegisterWrite;

TEST_CASE("FSidEmulator: synthetic 6502 program writes get captured by the tap", "[sid][emulator]")
{
	// Hand-rolled 6502 program at $1000:
	//   LDA #$42        A9 42         (2 cycles)
	//   STA $D404       8D 04 D4      (4 cycles, write to SID voice 1 freq lo)
	//   LDA #$11
	//   STA $D405       8D 05 D4      (4 cycles, write to SID voice 1 freq hi)
	//   JMP $100A       4C 0A 10      (3 cycles, infinite loop)
	static const uint8_t Program[] = {
		0xA9, 0x42,
		0x8D, 0x04, 0xD4,
		0xA9, 0x11,
		0x8D, 0x05, 0xD4,
		0x4C, 0x0A, 0x10,
	};

	FSidEmulator E;
	E.Reset(985248.0, 48000.0);
	E.LoadIntoRam(Program, sizeof(Program), 0x1000);
	E.BootCpu(0x1000);

	std::vector<FSidRegisterWrite> Writes;
	Writes.reserve(64);

	// Run a healthy number of audio samples — way more than the program
	// needs. If the CPU is genuinely fetching from $1000, the writes land
	// quickly; if not, they never land regardless of how long we run.
	for (uint16_t S = 0; S < 32; ++S)
	{
		E.TickOneAudioSample(Writes, S);
	}

	// Diagnostic: PC must be inside the JMP-loop at the end of the program
	// ($100A-$100C) once execution has settled.
	const uint16_t FinalPC = E.GetPC();
	INFO("Final PC: " << FinalPC);
	REQUIRE(FinalPC >= 0x1000);
	REQUIRE(FinalPC <= 0x100D);

	// Tap should have captured the two STA writes.
	bool bSawFreqLo = false;
	bool bSawFreqHi = false;
	for (const FSidRegisterWrite& W : Writes)
	{
		if (W.Reg == 0x04 && W.Value == 0x42) { bSawFreqLo = true; }
		if (W.Reg == 0x05 && W.Value == 0x11) { bSawFreqHi = true; }
	}
	REQUIRE(bSawFreqLo);
	REQUIRE(bSawFreqHi);
}

TEST_CASE("FSidEmulator: ticking after reset produces no register writes", "[sid][emulator]")
{
	// With zero RAM and PC set to a NOP loop, no SID writes should occur.
	// $00 = BRK; we'd hit reset vector at $FFFC-$FFFD which fetches PC from
	// uninitialised RAM (=0). The CPU lands at $0000 and runs zeros — BRK
	// instructions chain through the IRQ handler. None of that touches SID.
	FSidEmulator E;
	E.Reset(985248.0, 48000.0);

	std::vector<FSidRegisterWrite> Writes;
	for (uint16_t S = 0; S < 64; ++S)
	{
		E.TickOneAudioSample(Writes, S);
	}
	// Whatever the runaway CPU does, it shouldn't be writing to $D400-$D418.
	REQUIRE(Writes.empty());
}
