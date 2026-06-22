#pragma once

#include <cstdint>

namespace NodeSynth
{
	// MOS 6581/8580 register-file constants and conversions. The SID exposes
	// 25 write-only registers at $D400-$D418 (mirrored across $D400-$D7FF).
	// Register indices below are offsets into the file (0..24).
	namespace SidReg
	{
		// Per-voice base offsets — voice 1 starts at 0, voice 2 at 7, voice 3
		// at 14. Within each voice block:
		//   +0  Frequency low
		//   +1  Frequency high
		//   +2  Pulse-width low (12-bit total, low 8)
		//   +3  Pulse-width high (12-bit total, high 4 in low nibble)
		//   +4  Control register: gate (bit 0), sync (bit 1), ring-mod (bit 2),
		//                          test (bit 3), waveform select (bits 4-7)
		//   +5  Attack/Decay nibble pair
		//   +6  Sustain/Release nibble pair
		constexpr uint8_t Voice1Base = 0;
		constexpr uint8_t Voice2Base = 7;
		constexpr uint8_t Voice3Base = 14;

		constexpr uint8_t FreqLoOffset = 0;
		constexpr uint8_t FreqHiOffset = 1;
		constexpr uint8_t PwLoOffset = 2;
		constexpr uint8_t PwHiOffset = 3;
		constexpr uint8_t ControlOffset = 4;
		constexpr uint8_t AttackDecayOffset = 5;
		constexpr uint8_t SustainReleaseOffset = 6;

		// Global registers (filter + master).
		constexpr uint8_t FilterCutoffLo = 21;   // bits 0-2 used (lower 3 bits of cutoff)
		constexpr uint8_t FilterCutoffHi = 22;   // bits 0-7 (upper 8 bits of cutoff)
		constexpr uint8_t FilterResRouting = 23; // res in upper nibble, voice routing in lower 4 bits
		constexpr uint8_t FilterModeVolume = 24; // filter mode upper nibble, volume lower nibble

		// Control-register bit masks.
		constexpr uint8_t ControlGate = 0x01;
		constexpr uint8_t ControlSync = 0x02;
		constexpr uint8_t ControlRing = 0x04;
		constexpr uint8_t ControlTest = 0x08;
		constexpr uint8_t ControlTriangle = 0x10;
		constexpr uint8_t ControlSawtooth = 0x20;
		constexpr uint8_t ControlPulse = 0x40;
		constexpr uint8_t ControlNoise = 0x80;
		// Bits 4-7 expose four waveforms; encode them as a nibble (0..15) for
		// the V*n*_Waveform Control output. Triangle=1, Saw=2, Pulse=4, Noise=8.
		// (Multiple bits set means combined waveforms — ring-mod / sync / etc.)
		inline uint8_t WaveformBitsFromControl(uint8_t ControlByte)
		{
			return static_cast<uint8_t>((ControlByte >> 4) & 0x0F);
		}
	}

	// Convert a 16-bit SID frequency register pair to Hz at the given chip
	// clock. The SID frequency formula is `Hz = freg * Phi / 2^24`.
	inline float SidFreqToHz(uint8_t FregLo, uint8_t FregHi, double ChipClockHz)
	{
		const uint32_t Freg = (static_cast<uint32_t>(FregHi) << 8) | FregLo;
		return static_cast<float>((static_cast<double>(Freg) * ChipClockHz) / 16777216.0);
	}

	// Convert a 12-bit SID pulse-width pair to a 0..1 ratio. PW=0 means
	// "always low" (silent) and PW=4095 means "always high" — both produce
	// no audio. PW=2048 is a perfect 50% square.
	inline float SidPulseWidthRatio(uint8_t PwLo, uint8_t PwHi)
	{
		const uint16_t Pw = (static_cast<uint16_t>(PwHi & 0x0F) << 8) | PwLo;
		return static_cast<float>(Pw) / 4095.0f;
	}

	// MOS 6581 ADSR attack-time table, indexed by 4-bit nibble. Values are
	// the time taken to ramp from 0 to peak in milliseconds, taken straight
	// from the 6581 datasheet. Decay and Release use the same indexing but
	// the times are 3× the corresponding attack value (datasheet table 2).
	inline float SidAdsrAttackMs(uint8_t Nibble)
	{
		static constexpr float AttackTable[16] = {
			2.0f,    8.0f,    16.0f,   24.0f,
			38.0f,   56.0f,   68.0f,   80.0f,
			100.0f,  240.0f,  500.0f,  800.0f,
			1000.0f, 3000.0f, 5000.0f, 8000.0f
		};
		return AttackTable[Nibble & 0x0F];
	}

	inline float SidAdsrDecayReleaseMs(uint8_t Nibble)
	{
		// Datasheet says decay/release times are 3× the attack times.
		return SidAdsrAttackMs(Nibble) * 3.0f;
	}

	// Filter-cutoff register pair → 0..1 normalised. The 6581's actual cutoff
	// frequency is non-linear and chip-to-chip variable; surfacing Hz would
	// lie. Consumers can re-map with a Scale node if they want a specific
	// curve. The 11-bit value comes from FilterCutoffHi (8 bits) << 3 |
	// (FilterCutoffLo & 0x07).
	inline float SidCutoffNormalised(uint8_t Lo, uint8_t Hi)
	{
		const uint16_t Cutoff11 = (static_cast<uint16_t>(Hi) << 3) | (Lo & 0x07);
		return static_cast<float>(Cutoff11) / 2047.0f;
	}

	// Filter-resonance + voice-routing register: resonance in upper nibble,
	// per-voice filter routing flags in lower nibble.
	inline float SidResonanceNormalised(uint8_t ResRoutingByte)
	{
		return static_cast<float>((ResRoutingByte >> 4) & 0x0F) / 15.0f;
	}
	inline uint8_t SidFilterRoutingBits(uint8_t ResRoutingByte)
	{
		return static_cast<uint8_t>(ResRoutingByte & 0x0F);
	}

	// Filter mode + master volume register: mode in upper nibble (LP/BP/HP/
	// "voice 3 off"), volume in lower nibble.
	inline float SidVolumeNormalised(uint8_t ModeVolByte)
	{
		return static_cast<float>(ModeVolByte & 0x0F) / 15.0f;
	}
}
