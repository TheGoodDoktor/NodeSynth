#include "sid/SidEmulator.h"

#include <array>
#include <cmath>
#include <cstring>

// floooh/chips wants CHIPS_IMPL defined in exactly one translation unit per
// chip header. We're that one TU.
//
// We deliberately skip m6569 (VIC-II) — PSID's VBI mode just means "fire the
// play routine at 50/60 Hz", which is a cycle counter in FSidEmulator. A real
// VIC would only matter for RSID v3 tunes that actually display graphics or
// rely on raster-position-dependent IRQ timing, both deferred. m6569.h also
// uses C99 compound literals that don't compile under MSVC's C++ frontend.
#define CHIPS_IMPL
#include <chips_common.h>
#include <m6502.h>
#include <m6526.h>
#include <m6581.h>

namespace NodeSynth
{
	namespace
	{
		// CIA1 occupies $DC00-$DCFF, CIA2 $DD00-$DDFF, SID $D400-$D7FF.
		// Color RAM at $D800-$DBFF is an unused stub. $D000-$D3FF (VIC-II
		// register window) is treated as RAM for v1 — PSID tunes that probe
		// VIC see whatever was last written, which is "good enough" for the
		// vast majority of HVSC content.
		constexpr uint16_t SidBase = 0xD400;
		constexpr uint16_t SidEndExclusive = 0xD800;
		constexpr uint16_t Cia1Base = 0xDC00;
		constexpr uint16_t Cia2Base = 0xDD00;

		constexpr uint8_t SidRegisterCount = 25; // $D400-$D418

		// Bus-window selectors used to gate which chip's CS line we raise per
		// cycle. SID/CIA registers mirror across their range with the low bits
		// selecting the register (5 bits for SID, 4 for CIA, 6 for VIC).
		bool InRange(uint16_t Addr, uint16_t Base, uint16_t EndEx)
		{
			return Addr >= Base && Addr < EndEx;
		}
	}

	struct FSidEmulator::FImpl
	{
		std::array<uint8_t, 65536> Ram{};
		m6502_t Cpu{};
		m6526_t Cia1{};
		m6526_t Cia2{};
		m6581_t Sid{};
		uint64_t Pins = 0;
		double ChipClockHz = 985248.0;
		double SampleRateHz = 48000.0;
		// Number of CPU cycles per audio sample, fixed-point Q16. Recomputed
		// in Reset.
		uint32_t CyclesPerSampleQ16 = 0;
		uint32_t CycleAccumulatorQ16 = 0;
		bool bLastStepCompletedInstruction = false;

		// Virtual VBI timer: a cycle counter that pulses M6502_IRQ at a
		// configurable rate. Used for PSID VBI-mode tunes; the m6526 handles
		// CIA-mode IRQs natively so this stays disabled in that path.
		// VbiIrqHoldCycles is the per-pulse "IRQ stays asserted for N cycles
		// after VBI fires" countdown — needed because m6526_tick may clear
		// M6502_IRQ within the same cycle (OR-wired IRQ on real hardware,
		// each source individually pulls the line; here we have to defend
		// against CIA clearing the bit). 16 cycles gives the CPU plenty of
		// time to finish its current instruction and start the IRQ sequence.
		bool bVbiEnabled = false;
		uint32_t VbiCyclesPerTick = 0;
		uint32_t VbiCycleCounter = 0;
		uint32_t VbiIrqHoldCycles = 0;

		// IRQ-source latches captured on each chip's tick, OR'd back into the
		// pin word at the start of the next cycle. Without this, m6526's
		// "always rewrite IRQ bit based on my own state" behaviour silently
		// suppresses concurrent IRQ sources — e.g. Cia2's tick clears Cia1's
		// IRQ assertion when Cia2 has nothing pending. Real hardware OR-wires
		// IRQs from all chips; we emulate that here.
		bool bCia1IrqLatched = false;
		bool bCia2IrqLatched = false;

		void TickOneCycle(std::vector<FSidRegisterWrite>* OutWrites,
			uint16_t SampleOffset, float* AccumSample, uint32_t* AccumCount)
		{
			// 0) Virtual VBI timer (PSID VBI-mode interrupt source). Each tick
			//    increments the counter; when it crosses VbiCyclesPerTick
			//    we trigger a hold-down of M6502_IRQ for ~16 cycles so the
			//    CPU has time to finish its current instruction and start
			//    servicing the interrupt.
			bool bVbiWants = false;
			if (bVbiEnabled)
			{
				if (++VbiCycleCounter >= VbiCyclesPerTick)
				{
					VbiCycleCounter = 0;
					VbiIrqHoldCycles = 16;
				}
			}
			if (VbiIrqHoldCycles > 0)
			{
				bVbiWants = true;
				--VbiIrqHoldCycles;
			}

			// OR all IRQ sources together before m6502_tick. The CIA latches
			// from the previous cycle drive this; their fresh values are
			// captured below after each m6526_tick.
			const bool bAnyIrq = bVbiWants || bCia1IrqLatched || bCia2IrqLatched;
			if (bAnyIrq) { Pins |= M6502_IRQ; }
			else         { Pins &= ~M6502_IRQ; }

			// 1) Tick the CPU. It puts an address + RW on the pin word; on
			//    writes it also drives the data bus.
			Pins = m6502_tick(&Cpu, Pins);
			bLastStepCompletedInstruction = (Pins & M6502_SYNC) != 0;

			const uint16_t Addr = M6502_GET_ADDR(Pins);
			const bool bRead = (Pins & M6502_RW) != 0;

			// 2) Decide which chip (if any) the CPU is talking to and raise
			//    that chip's CS. Clear the CS bits for chips not selected so
			//    they tick without responding to the bus.
			Pins &= ~(M6581_CS | M6526_CS);

			const bool bSidSel = InRange(Addr, SidBase, SidEndExclusive);
			const bool bCia1Sel = InRange(Addr, Cia1Base, Cia1Base + 0x100);
			const bool bCia2Sel = InRange(Addr, Cia2Base, Cia2Base + 0x100);

			// 3) Tap CPU writes to the SID register file BEFORE the SID
			//    consumes them on its tick. The tap records the intended
			//    write; the SID processes it on its own clock.
			if (bSidSel && !bRead && OutWrites != nullptr)
			{
				const uint8_t Reg = static_cast<uint8_t>(Addr & 0x1F);
				if (Reg < SidRegisterCount)
				{
					FSidRegisterWrite W;
					W.Reg = Reg;
					W.Value = M6502_GET_DATA(Pins);
					W.SampleOffset = SampleOffset;
					OutWrites->push_back(W);
				}
			}

			// 4) Tick each peripheral chip. With CS clear they advance state
			//    (timers, oscillator phases) without bus IO. With CS set
			//    they consume / produce a data-bus byte this cycle.
			if (bSidSel) { Pins |= M6581_CS; }
			Pins = m6581_tick(&Sid, Pins);
			if (Pins & M6581_SAMPLE)
			{
				if (AccumSample != nullptr) { *AccumSample += Sid.sample; }
				if (AccumCount != nullptr) { *AccumCount += 1; }
			}

			// CIA ticks: clear M6502_IRQ before each so we can capture this
			// chip's IRQ output cleanly. m6526 unconditionally rewrites the
			// bit from its own state, so without isolation the second CIA
			// would clobber the first's assertion.
			Pins &= ~M6526_CS;
			if (bCia1Sel) { Pins |= M6526_CS; }
			Pins &= ~M6502_IRQ;
			Pins = m6526_tick(&Cia1, Pins);
			bCia1IrqLatched = (Pins & M6502_IRQ) != 0;

			Pins &= ~M6526_CS;
			if (bCia2Sel) { Pins |= M6526_CS; }
			Pins &= ~M6502_IRQ;
			Pins = m6526_tick(&Cia2, Pins);
			bCia2IrqLatched = (Pins & M6502_IRQ) != 0;

			// 5) RAM dispatch for any address that didn't hit a peripheral.
			//    Color RAM ($D800-$DBFF) is a stub. $D000-$D3FF (would-be VIC
			//    register window) and $DE00-$DFFF (I/O extension area) are
			//    backed by RAM in v1; tunes that probe those see whatever
			//    was last written, which works for HVSC content.
			if (!bSidSel && !bCia1Sel && !bCia2Sel)
			{
				if (Addr >= 0xD800 && Addr < 0xDC00)
				{
					if (bRead) { M6502_SET_DATA(Pins, 0); }
					// writes silently dropped
				}
				else
				{
					if (bRead)
					{
						M6502_SET_DATA(Pins, Ram[Addr]);
					}
					else
					{
						Ram[Addr] = M6502_GET_DATA(Pins);
					}
				}
			}
		}
	};

	FSidEmulator::FSidEmulator()
		: Impl(std::make_unique<FImpl>())
	{
	}

	FSidEmulator::~FSidEmulator() = default;

	void FSidEmulator::Reset(double ChipClockHz, double SampleRateHz)
	{
		Impl->Ram.fill(0);
		Impl->ChipClockHz = ChipClockHz;
		Impl->SampleRateHz = SampleRateHz;
		Impl->CyclesPerSampleQ16 = static_cast<uint32_t>(
			(ChipClockHz / SampleRateHz) * 65536.0);
		Impl->CycleAccumulatorQ16 = 0;
		Impl->bCia1IrqLatched = false;
		Impl->bCia2IrqLatched = false;
		Impl->VbiIrqHoldCycles = 0;
		Impl->VbiCycleCounter = 0;

		// Init CPU. Setting M6502_BCD_DISABLED is fine for SID tunes (they
		// don't generally use decimal mode and the C64 6510 has it).
		m6502_desc_t CpuDesc{};
		Impl->Pins = m6502_init(&Impl->Cpu, &CpuDesc);

		// Init peripheral chips.
		m6526_init(&Impl->Cia1);
		m6526_init(&Impl->Cia2);

		m6581_desc_t SidDesc{};
		SidDesc.tick_hz = static_cast<int>(ChipClockHz);
		SidDesc.sound_hz = static_cast<int>(SampleRateHz);
		SidDesc.magnitude = 1.0f;
		m6581_init(&Impl->Sid, &SidDesc);

		// Pulse the reset line through several cycles so the CPU runs its
		// reset sequence and arrives at PC = ($FFFC-$FFFD). With RAM zeroed
		// the CPU lands at $0000 — a placeholder. Callers wire up the actual
		// entry point via BootCpu after loading their program.
		Impl->Pins |= M6502_RES;
		for (int I = 0; I < 8; ++I)
		{
			Impl->TickOneCycle(nullptr, 0, nullptr, nullptr);
		}
		Impl->Pins &= ~M6502_RES;
	}

	void FSidEmulator::BootCpu(uint16_t StartAddr)
	{
		// The 6502's reset sequence loads PC from ($FFFC-$FFFD). Wiring the
		// vector to StartAddr and pulsing RES is the only race-free way to
		// bring the CPU into our code; m6502_set_pc fights the pipelined
		// reset-vector fetch for the first few cycles after Reset.
		Impl->Ram[0xFFFC] = static_cast<uint8_t>(StartAddr & 0xFF);
		Impl->Ram[0xFFFD] = static_cast<uint8_t>((StartAddr >> 8) & 0xFF);

		Impl->Pins |= M6502_RES;
		for (int I = 0; I < 9; ++I)
		{
			Impl->TickOneCycle(nullptr, 0, nullptr, nullptr);
		}
		Impl->Pins &= ~M6502_RES;
		// One more tick after RES release lets the CPU latch the new PC and
		// start fetching at StartAddr on the next caller-driven tick.
		Impl->TickOneCycle(nullptr, 0, nullptr, nullptr);
	}

	void FSidEmulator::LoadIntoRam(const uint8_t* Data, size_t Size, uint16_t LoadAddr)
	{
		if (Data == nullptr || Size == 0) { return; }
		const size_t Available = 0x10000 - static_cast<size_t>(LoadAddr);
		const size_t Copy = (Size < Available) ? Size : Available;
		std::memcpy(Impl->Ram.data() + LoadAddr, Data, Copy);
	}

	uint8_t FSidEmulator::PeekRam(uint16_t Addr) const { return Impl->Ram[Addr]; }
	void FSidEmulator::PokeRam(uint16_t Addr, uint8_t Value) { Impl->Ram[Addr] = Value; }

	void FSidEmulator::SetA(uint8_t V)  { m6502_set_a(&Impl->Cpu, V); }
	void FSidEmulator::SetX(uint8_t V)  { m6502_set_x(&Impl->Cpu, V); }
	void FSidEmulator::SetY(uint8_t V)  { m6502_set_y(&Impl->Cpu, V); }
	void FSidEmulator::SetPC(uint16_t V){ m6502_set_pc(&Impl->Cpu, V); }
	void FSidEmulator::SetS(uint8_t V)  { m6502_set_s(&Impl->Cpu, V); }
	uint8_t FSidEmulator::GetA() const  { return m6502_a(const_cast<m6502_t*>(&Impl->Cpu)); }
	uint16_t FSidEmulator::GetPC() const{ return m6502_pc(const_cast<m6502_t*>(&Impl->Cpu)); }

	float FSidEmulator::TickOneAudioSample(
		std::vector<FSidRegisterWrite>& OutWrites,
		uint16_t SampleOffset)
	{
		// Run integer cycles per sample, plus a fractional carry so we don't
		// drift on non-integer ratios (e.g. PAL at 985248 / 48000).
		Impl->CycleAccumulatorQ16 += Impl->CyclesPerSampleQ16;
		const uint32_t WholeCycles = Impl->CycleAccumulatorQ16 >> 16;
		Impl->CycleAccumulatorQ16 &= 0xFFFF;

		float AccumSample = 0.0f;
		uint32_t AccumCount = 0;
		for (uint32_t I = 0; I < WholeCycles; ++I)
		{
			Impl->TickOneCycle(&OutWrites, SampleOffset, &AccumSample, &AccumCount);
		}
		// m6581 emits SAMPLE-true only on cycles its internal sample timer
		// fires. AccumCount is typically 0 or 1 per WholeCycles batch (the
		// chip is configured for sound_hz == NodeSynth sample rate). When
		// 0 we hold the previous value at zero — silence between samples is
		// rare but possible at the very start.
		return (AccumCount > 0) ? (AccumSample / static_cast<float>(AccumCount)) : 0.0f;
	}

	uint32_t FSidEmulator::StepInstruction()
	{
		// Run cycles until SYNC (next instruction) is asserted. SYNC fires on
		// the first cycle of each new instruction, so we step at least once
		// then keep going while SYNC stays low.
		uint32_t Cycles = 0;
		do
		{
			Impl->TickOneCycle(nullptr, 0, nullptr, nullptr);
			++Cycles;
		}
		while (!(Impl->Pins & M6502_SYNC) && Cycles < 16);
		return Cycles;
	}

	bool FSidEmulator::LastStepCompletedInstruction() const
	{
		return Impl->bLastStepCompletedInstruction;
	}

	bool FSidEmulator::RunInitRoutine(uint16_t InitAddr, uint8_t Subtune, uint32_t MaxInstructions)
	{
		// After init RTSes, we want the CPU to spin idly until the next IRQ
		// fires — *not* execute BRKs out of zeroed RAM, because BRK shares
		// the IRQ vector and would re-enter the play stub at CPU rate
		// (thousands of Hz) instead of the intended VBI rate (50/60 Hz).
		// Solution: install a `JMP $0002` infinite loop at $0002 and push
		// $0001 as the sentinel return so init's RTS pops to $0002.
		//   $0002  4C 02 00   JMP $0002
		Impl->Ram[0x0002] = 0x4C;
		Impl->Ram[0x0003] = 0x02;
		Impl->Ram[0x0004] = 0x00;
		// Push sentinel = $0001 onto the stack (high byte first per 6502
		// convention; RTS pops low then high then increments PC by 1).
		Impl->Ram[0x01FD] = 0x00;  // high byte of $0001
		Impl->Ram[0x01FC] = 0x01;  // low  byte of $0001
		m6502_set_s(&Impl->Cpu, 0xFB);

		// Default IRQ vector to the JMP-self loop too — if no play hook is
		// installed yet (or init enables IRQs early), we won't run garbage.
		Impl->Ram[0xFFFE] = 0x02;
		Impl->Ram[0xFFFF] = 0x00;

		m6502_set_a(&Impl->Cpu, Subtune);
		m6502_set_x(&Impl->Cpu, 0);
		m6502_set_y(&Impl->Cpu, 0);
		m6502_set_pc(&Impl->Cpu, InitAddr);

		// Step instruction-by-instruction until PC drops into the sentinel
		// region or we exceed the budget. After init's RTS, PC == $0002
		// (the JMP-self landing); also accept anything in zero page in case
		// init RTI'd or jumped wild.
		for (uint32_t I = 0; I < MaxInstructions; ++I)
		{
			StepInstruction();
			const uint16_t PC = m6502_pc(&Impl->Cpu);
			if (PC < 0x0100)
			{
				// Force the I (interrupt-disable) flag clear before play
				// starts firing. PSID spec assumes init does CLI before
				// returning, but plenty of HVSC tunes don't — and with the
				// old BRK-loop bug they'd still play (BRK ignores I). Now
				// that play only fires through real IRQ delivery, an init
				// that leaves I set means total silence. Match libsidplayfp:
				// always force I=0 here regardless of what init did.
				m6502_set_p(&Impl->Cpu, m6502_p(&Impl->Cpu) & ~0x04);
				return true;
			}
		}
		return false;
	}

	void FSidEmulator::InstallPlayHook(uint16_t PlayAddr)
	{
		// Stub at $FFE0:
		//   $FFE0  20 lo hi  JSR PlayAddr
		//   $FFE3  40        RTI
		// Then point the IRQ vector at the stub.
		Impl->Ram[0xFFE0] = 0x20;
		Impl->Ram[0xFFE1] = static_cast<uint8_t>(PlayAddr & 0xFF);
		Impl->Ram[0xFFE2] = static_cast<uint8_t>((PlayAddr >> 8) & 0xFF);
		Impl->Ram[0xFFE3] = 0x40;
		Impl->Ram[0xFFFE] = 0xE0;
		Impl->Ram[0xFFFF] = 0xFF;
	}

	void FSidEmulator::SetVbiTimer(bool bEnabled, double TickRateHz)
	{
		Impl->bVbiEnabled = bEnabled;
		if (bEnabled && TickRateHz > 0.0)
		{
			Impl->VbiCyclesPerTick = static_cast<uint32_t>(Impl->ChipClockHz / TickRateHz);
		}
		else
		{
			Impl->VbiCyclesPerTick = 0;
		}
		Impl->VbiCycleCounter = 0;
	}
}
