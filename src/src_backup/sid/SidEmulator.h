#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace NodeSynth
{
	// One captured CPU write to the SID register file at $D400-$D418. The tap
	// records every write that lands in the SID address range each tick;
	// FSidPlayer turns the stream into Control output buffers.
	struct FSidRegisterWrite
	{
		uint8_t Reg = 0;          // 0..24 (offset within $D400-$D418)
		uint8_t Value = 0;
		uint16_t SampleOffset = 0; // 0..(BlockSize-1) within the current Process call
	};

	// PSID timer mode — which interrupt source drives the play routine.
	// Selected from the .sid header's `speed` field per subtune. Used by
	// FPsidLoader to arm the right chip; FSidEmulator is agnostic and just
	// runs whatever timer the loader programmed in RAM.
	enum class EPsidTimer : uint8_t { Vbi, Cia };

	// Owns the floooh/chips emulator state for the C64 hardware needed to run
	// PSID v1/v2 tunes: m6502 CPU + m6526 ×2 (CIA1, CIA2) + m6569 (VIC-II) +
	// m6581 (SID) + 64 KB of RAM. Exposes a tick-per-audio-sample interface
	// that captures SID register writes and emits one audio sample.
	//
	// PIMPL'd to keep the C-style chips/ headers out of the public surface,
	// which keeps NodeSynth's other translation units allocation-warning-free.
	class FSidEmulator
	{
	public:
		FSidEmulator();
		~FSidEmulator();

		FSidEmulator(const FSidEmulator&) = delete;
		FSidEmulator& operator=(const FSidEmulator&) = delete;

		// Reset CPU + chips, zero RAM. Call before any tick / load.
		// `ChipClockHz` is 985248 for PAL or 1022727 for NTSC (the C64 master
		// clock for the chosen region). `SampleRateHz` is NodeSynth's current
		// audio sample rate. m6581 internally produces samples at SampleRateHz.
		void Reset(double ChipClockHz, double SampleRateHz);

		// Pokes the 6502 reset vector at $FFFC-$FFFD to point at StartAddr,
		// then pulses RES through enough cycles for the CPU to fetch the
		// vector and land at StartAddr ready to execute. Use this in
		// preference to SetPC for entering loaded code — SetPC works
		// mid-instruction at best and races the CPU's pipelined fetch.
		void BootCpu(uint16_t StartAddr);

		// Run the PSID init routine. Sets A = Subtune, X = Y = 0, PC = InitAddr,
		// and pushes a sentinel return address onto the stack so the init's
		// final RTS lands at PC = $0000 (which we detect to know it returned).
		// Steps the CPU instruction-by-instruction up to MaxInstructions; on
		// timeout, returns false. Returns true on clean RTS-to-sentinel.
		bool RunInitRoutine(uint16_t InitAddr, uint8_t Subtune,
			uint32_t MaxInstructions = 1000000);

		// Install a small stub at $FFE0 that does `JSR PlayAddr / RTI`, and
		// point the IRQ vector ($FFFE-$FFFF) at it. Now any CPU IRQ trigger
		// runs the play routine and returns to whatever was executing before.
		void InstallPlayHook(uint16_t PlayAddr);

		// Configure the virtual VBI interrupt source — a cycle counter that
		// pulses M6502_IRQ at the given rate. For PSID's VBI-mode tunes,
		// 50 Hz (PAL) or 60 Hz (NTSC). Disable (bEnabled=false) for CIA-mode
		// tunes — the m6526 chip raises its own IRQs from the player code's
		// timer programming.
		void SetVbiTimer(bool bEnabled, double TickRateHz);

		// Copy raw bytes into the 64 KB RAM array starting at LoadAddr.
		// Wraps if Size + LoadAddr exceeds 0x10000 (with a clamp; defensive
		// against oversized PSID payloads).
		void LoadIntoRam(const uint8_t* Data, size_t Size, uint16_t LoadAddr);

		// Direct RAM peek/poke for the loader (e.g. installing the IRQ stub).
		// Not called from the audio thread.
		uint8_t PeekRam(uint16_t Addr) const;
		void PokeRam(uint16_t Addr, uint8_t Value);

		// CPU register access (used by the loader to set A=subtune, etc.).
		void SetA(uint8_t V);
		void SetX(uint8_t V);
		void SetY(uint8_t V);
		void SetPC(uint16_t V);
		void SetS(uint8_t V);
		uint8_t GetA() const;
		uint16_t GetPC() const;

		// Tick the CPU (and all peripheral chips) for exactly the number of
		// cycles in one audio sample, dispatching all bus reads/writes.
		// Captures every CPU write to $D400-$D418 into OutWrites, with the
		// passed SampleOffset stamped on each capture. Returns one audio
		// sample as accumulated from m6581's per-tick output.
		float TickOneAudioSample(std::vector<FSidRegisterWrite>& OutWrites,
			uint16_t SampleOffset);

		// Single-CPU-instruction step. Returns the number of cycles consumed
		// (always >= 1). Used by the loader to bound init-routine execution.
		// Bus reads/writes are dispatched, but no audio sample is produced
		// (the chip emulators tick alongside the CPU but their sample output
		// is discarded — init is expected to be brief).
		uint32_t StepInstruction();

		// True iff the most recent StepInstruction completed an instruction
		// boundary (SYNC pin asserted). Useful for "run until next sentinel
		// instruction" logic in the loader.
		bool LastStepCompletedInstruction() const;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
