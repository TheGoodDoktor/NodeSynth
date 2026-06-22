#include "dsp/SidPlayer.h"

#include <algorithm>
#include <cstring>

#include "io/SidBrowser.h"
#include "sid/PsidLoader.h"
#include "sid/SidEmulator.h"
#include "sid/SidRegisters.h"

namespace NodeSynth
{
	namespace
	{
		// 5 ms one-pole smoother time constant for smoothed-class Control
		// outputs (Freq, PWM, Cutoff, Resonance, Volume). Short enough that
		// register-rate updates (every ~20 ms at 50 Hz PSID tick) don't lag
		// audibly; long enough to mask zipper noise on slider-rate sweeps.
		constexpr float SmootherTimeConstantMs = 5.0f;

		// Convenience: voice-block output indices keyed by per-voice base.
		struct FVoiceBlock
		{
			uint32_t Freq, PWM, Gate, Waveform, Attack, Decay, Sustain, Release;
		};
		constexpr FVoiceBlock VoiceBlocks[3] = {
			{
				FSidPlayer::Output_V1_Freq, FSidPlayer::Output_V1_PWM,
				FSidPlayer::Output_V1_Gate, FSidPlayer::Output_V1_Waveform,
				FSidPlayer::Output_V1_Attack, FSidPlayer::Output_V1_Decay,
				FSidPlayer::Output_V1_Sustain, FSidPlayer::Output_V1_Release,
			},
			{
				FSidPlayer::Output_V2_Freq, FSidPlayer::Output_V2_PWM,
				FSidPlayer::Output_V2_Gate, FSidPlayer::Output_V2_Waveform,
				FSidPlayer::Output_V2_Attack, FSidPlayer::Output_V2_Decay,
				FSidPlayer::Output_V2_Sustain, FSidPlayer::Output_V2_Release,
			},
			{
				FSidPlayer::Output_V3_Freq, FSidPlayer::Output_V3_PWM,
				FSidPlayer::Output_V3_Gate, FSidPlayer::Output_V3_Waveform,
				FSidPlayer::Output_V3_Attack, FSidPlayer::Output_V3_Decay,
				FSidPlayer::Output_V3_Sustain, FSidPlayer::Output_V3_Release,
			},
		};

		// True for outputs that smooth toward their target value (vs. step).
		bool IsSmoothedOutput(uint32_t Out)
		{
			switch (Out)
			{
				case FSidPlayer::Output_V1_Freq:
				case FSidPlayer::Output_V1_PWM:
				case FSidPlayer::Output_V2_Freq:
				case FSidPlayer::Output_V2_PWM:
				case FSidPlayer::Output_V3_Freq:
				case FSidPlayer::Output_V3_PWM:
				case FSidPlayer::Output_F_Cutoff:
				case FSidPlayer::Output_F_Resonance:
				case FSidPlayer::Output_Volume:
					return true;
				default:
					return false;
			}
		}
	}

	FSidPlayer::FSidPlayer() = default;
	FSidPlayer::~FSidPlayer() = default;

	std::vector<FPortInfo> FSidPlayer::GetOutputPorts() const
	{
		std::vector<FPortInfo> Ports;
		Ports.reserve(Output_COUNT);
		Ports.push_back({ "Out", EPortType::Audio, "Mono SID audio. Stereo in v1 is wire-broadcast." });
		const char* VoiceNames[3] = { "V1", "V2", "V3" };
		for (int V = 0; V < 3; ++V)
		{
			const std::string Vn = VoiceNames[V];
			Ports.push_back({ Vn + "_Freq",      EPortType::Control, "Voice " + Vn + " oscillator frequency in Hz, smoothed." });
			Ports.push_back({ Vn + "_PWM",       EPortType::Control, "Voice " + Vn + " pulse-width 0..1, smoothed." });
			Ports.push_back({ Vn + "_Gate",      EPortType::Control, "Voice " + Vn + " gate (0/1)." });
			Ports.push_back({ Vn + "_Waveform",  EPortType::Control, "Voice " + Vn + " waveform bitmask (Tri=1 Saw=2 Pulse=4 Noise=8)." });
			Ports.push_back({ Vn + "_Attack",    EPortType::Control, "Voice " + Vn + " ADSR attack in ms." });
			Ports.push_back({ Vn + "_Decay",     EPortType::Control, "Voice " + Vn + " ADSR decay in ms." });
			Ports.push_back({ Vn + "_Sustain",   EPortType::Control, "Voice " + Vn + " ADSR sustain level 0..1." });
			Ports.push_back({ Vn + "_Release",   EPortType::Control, "Voice " + Vn + " ADSR release in ms." });
		}
		Ports.push_back({ "F_Cutoff",    EPortType::Control, "Filter cutoff 0..1 (normalised; 6581 cutoff curve is non-linear)." });
		Ports.push_back({ "F_Resonance", EPortType::Control, "Filter resonance 0..1." });
		Ports.push_back({ "F_Routing",   EPortType::Control, "Filter routing bitmask (V1=1 V2=2 V3=4 ExtIn=8)." });
		Ports.push_back({ "Volume",      EPortType::Control, "Master volume 0..1, smoothed." });
		return Ports;
	}

	std::vector<FParamInfo> FSidPlayer::GetParamInfos() const
	{
		std::vector<FParamInfo> Infos;
		Infos.reserve(Param_COUNT);
		{
			FParamInfo File{};
			File.Name = "File";
			File.Kind = EParamKind::String;
			File.Description =
				"Path to a .sid file (relative to sidfiles/ or absolute).\n"
				"The property panel surfaces a dropdown of bundled tunes.";
			File.bHidden = true;  // surfaced by the custom UI dropdown
			Infos.push_back(std::move(File));
		}
		Infos.push_back({ "Subtune", 1.0f, 32.0f, 1.0f, false, EParamKind::Float, {},
			"1-based subtune index. Out-of-range values are clamped to the file's actual subtune count." });
		Infos.push_back({ "Region", 0.0f, 1.0f, 0.0f, false, EParamKind::Choice,
			{ "PAL", "NTSC" },
			"C64 region (chip clock). Defaults to PAL; PSID header may suggest otherwise." });
		Infos.push_back({ "Model", 0.0f, 1.0f, 0.0f, false, EParamKind::Choice,
			{ "6581", "8580" },
			"SID model. Informational in v1 — m6581 emulates a single curve regardless." });
		Infos.push_back({ "Bypass", 0.0f, 1.0f, 0.0f, false, EParamKind::Bool, {},
			"When true, audio output is silent and Control outputs hold zero." });
		return Infos;
	}

	float FSidPlayer::GetParamValue(uint32_t Index) const
	{
		switch (Index)
		{
			case Param_Subtune: return ParamSubtune.load(std::memory_order_relaxed);
			case Param_Region:  return ParamRegion.load(std::memory_order_relaxed);
			case Param_Model:   return ParamModel.load(std::memory_order_relaxed);
			case Param_Bypass:  return ParamBypass.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
			default: return 0.0f;
		}
	}

	void FSidPlayer::SetParamValue(uint32_t Index, float Value)
	{
		switch (Index)
		{
			case Param_Subtune:
			{
				const float Clamped = std::max(1.0f, std::min(32.0f, Value));
				const float Old = ParamSubtune.exchange(Clamped, std::memory_order_relaxed);
				if (Clamped != Old) { RebuildEmulator(); }
				break;
			}
			case Param_Region:
			{
				const float Clamped = (Value >= 0.5f) ? 1.0f : 0.0f;
				const float Old = ParamRegion.exchange(Clamped, std::memory_order_relaxed);
				if (Clamped != Old) { RebuildEmulator(); }
				break;
			}
			case Param_Model:
			{
				const float Clamped = (Value >= 0.5f) ? 1.0f : 0.0f;
				ParamModel.store(Clamped, std::memory_order_relaxed);
				// Model change is informational only in v1 — no rebuild needed.
				break;
			}
			case Param_Bypass:
			{
				ParamBypass.store(Value >= 0.5f, std::memory_order_relaxed);
				break;
			}
			default: break;
		}
	}

	std::string FSidPlayer::GetParamString(uint32_t Index) const
	{
		if (Index == Param_File)
		{
			std::lock_guard<std::mutex> Lock(InfoMutex);
			return FilePath;
		}
		return {};
	}

	void FSidPlayer::SetParamString(uint32_t Index, const std::string& Value)
	{
		if (Index != Param_File) { return; }
		{
			std::lock_guard<std::mutex> Lock(InfoMutex);
			FilePath = Value;
		}
		RebuildEmulator();
	}

	void FSidPlayer::Prepare(double InSampleRate)
	{
		SampleRate = InSampleRate;
		for (uint32_t I = 0; I < Output_COUNT; ++I)
		{
			Smoothers[I].Prepare(InSampleRate, SmootherTimeConstantMs);
			Smoothers[I].Reset(0.0f);
			LastValue[I] = 0.0f;
		}
		// If a tune was loaded before Prepare, rebuild the emulator now so it
		// uses the correct sample rate (chip-cycles-per-audio-sample ratio
		// depends on it).
		std::string PathCopy;
		{
			std::lock_guard<std::mutex> Lock(InfoMutex);
			PathCopy = FilePath;
		}
		if (!PathCopy.empty())
		{
			RebuildEmulator();
		}
	}

	float* FSidPlayer::GetOutputBuffer(uint32_t Index, uint32_t Channel)
	{
		if (Index >= Output_COUNT || Channel >= NumChannels) { return nullptr; }
		return OutputBuffers[Index][Channel];
	}

	void FSidPlayer::Process(const FProcessContext& Ctx)
	{
		// Bypass: zero everything out and bail.
		if (ParamBypass.load(std::memory_order_relaxed))
		{
			for (uint32_t Out = 0; Out < Output_COUNT; ++Out)
			{
				std::memset(OutputBuffers[Out][0], 0, sizeof(float) * Ctx.BlockSize);
			}
			return;
		}

		auto Snap = std::atomic_load_explicit(&ActiveEmulator, std::memory_order_acquire);
		if (!Snap)
		{
			// No tune loaded — silence + hold last known Control values.
			std::memset(OutputBuffers[Output_Audio][0], 0, sizeof(float) * Ctx.BlockSize);
			for (uint32_t Out = 1; Out < Output_COUNT; ++Out)
			{
				const float Val = LastValue[Out];
				for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
				{
					OutputBuffers[Out][0][I] = Val;
				}
			}
			return;
		}

		// Audio buffer + per-sample tap dispatch.
		float* Audio = OutputBuffers[Output_Audio][0];
		std::vector<FSidRegisterWrite> Writes;
		Writes.reserve(64);

		double ChipClockHz;
		{
			std::lock_guard<std::mutex> Lock(InfoMutex);
			ChipClockHz = Info.ChipClockHz;
		}

		for (uint32_t I = 0; I < Ctx.BlockSize; ++I)
		{
			Writes.clear();
			Audio[I] = Snap->TickOneAudioSample(Writes, static_cast<uint16_t>(I));

			// Walk captured writes; each one updates the LastValue for
			// affected outputs. Smoothed outputs slide in this sample's loop;
			// step outputs latch.
			for (const FSidRegisterWrite& W : Writes)
			{
				// Decode which voice (if any) this write hits.
				int VoiceIdx = -1;
				uint8_t VoiceReg = 0;
				if (W.Reg < 7)              { VoiceIdx = 0; VoiceReg = W.Reg; }
				else if (W.Reg < 14)        { VoiceIdx = 1; VoiceReg = W.Reg - 7; }
				else if (W.Reg < 21)        { VoiceIdx = 2; VoiceReg = W.Reg - 14; }
				if (VoiceIdx >= 0)
				{
					const FVoiceBlock& B = VoiceBlocks[VoiceIdx];
					const uint8_t Voice = static_cast<uint8_t>(VoiceIdx);
					switch (VoiceReg)
					{
						case SidReg::FreqLoOffset:
						case SidReg::FreqHiOffset:
						{
							// freq is a 16-bit pair across regs 0+1; cache
							// the byte and recompute Hz from both halves.
							CachedRegs[Voice][VoiceReg] = W.Value;
							const float Hz = SidFreqToHz(
								CachedRegs[Voice][SidReg::FreqLoOffset],
								CachedRegs[Voice][SidReg::FreqHiOffset],
								ChipClockHz);
							LastValue[B.Freq] = Hz;
							break;
						}
						case SidReg::PwLoOffset:
						case SidReg::PwHiOffset:
						{
							CachedRegs[Voice][VoiceReg] = W.Value;
							const float Pw = SidPulseWidthRatio(
								CachedRegs[Voice][SidReg::PwLoOffset],
								CachedRegs[Voice][SidReg::PwHiOffset]);
							LastValue[B.PWM] = Pw;
							break;
						}
						case SidReg::ControlOffset:
						{
							CachedRegs[Voice][VoiceReg] = W.Value;
							LastValue[B.Gate] = (W.Value & SidReg::ControlGate) ? 1.0f : 0.0f;
							LastValue[B.Waveform] = static_cast<float>(SidReg::WaveformBitsFromControl(W.Value));
							break;
						}
						case SidReg::AttackDecayOffset:
						{
							CachedRegs[Voice][VoiceReg] = W.Value;
							const uint8_t A = (W.Value >> 4) & 0x0F;
							const uint8_t D = W.Value & 0x0F;
							LastValue[B.Attack] = SidAdsrAttackMs(A);
							LastValue[B.Decay] = SidAdsrDecayReleaseMs(D);
							break;
						}
						case SidReg::SustainReleaseOffset:
						{
							CachedRegs[Voice][VoiceReg] = W.Value;
							const uint8_t S = (W.Value >> 4) & 0x0F;
							const uint8_t R = W.Value & 0x0F;
							LastValue[B.Sustain] = static_cast<float>(S) / 15.0f;
							LastValue[B.Release] = SidAdsrDecayReleaseMs(R);
							break;
						}
					}
				}
				else
				{
					// Global registers.
					switch (W.Reg)
					{
						case SidReg::FilterCutoffLo:
						case SidReg::FilterCutoffHi:
						{
							CachedGlobals[W.Reg - SidReg::FilterCutoffLo] = W.Value;
							LastValue[Output_F_Cutoff] = SidCutoffNormalised(
								CachedGlobals[0], CachedGlobals[1]);
							break;
						}
						case SidReg::FilterResRouting:
						{
							CachedGlobals[2] = W.Value;
							LastValue[Output_F_Resonance] = SidResonanceNormalised(W.Value);
							LastValue[Output_F_Routing] = static_cast<float>(SidFilterRoutingBits(W.Value));
							break;
						}
						case SidReg::FilterModeVolume:
						{
							CachedGlobals[3] = W.Value;
							LastValue[Output_Volume] = SidVolumeNormalised(W.Value);
							break;
						}
					}
				}
			}

			// Per-sample fill: smoothed outputs slide; step outputs latch.
			for (uint32_t Out = 1; Out < Output_COUNT; ++Out)
			{
				if (IsSmoothedOutput(Out))
				{
					Smoothers[Out].SetTarget(LastValue[Out]);
					OutputBuffers[Out][0][I] = Smoothers[Out].Tick();
				}
				else
				{
					OutputBuffers[Out][0][I] = LastValue[Out];
				}
			}
		}
	}

	void FSidPlayer::RebuildEmulator()
	{
		std::string Path;
		uint16_t Subtune;
		bool bRegionNtscOverride;
		{
			std::lock_guard<std::mutex> Lock(InfoMutex);
			Path = FilePath;
			Subtune = static_cast<uint16_t>(ParamSubtune.load(std::memory_order_relaxed));
			bRegionNtscOverride = ParamRegion.load(std::memory_order_relaxed) >= 0.5f;
			LoadError.clear();
		}
		if (Path.empty())
		{
			std::atomic_store_explicit(&ActiveEmulator,
				std::shared_ptr<FSidEmulator>{},
				std::memory_order_release);
			return;
		}

		// Resolve relative-to-bundled / user paths into an absolute path
		// the SID loader can open. Falls through to the stored string
		// (which the loader will then reject) so the error message points
		// at what the user actually typed.
		std::filesystem::path Resolved = ResolveSidPath(Path);
		const std::string LoadPath = Resolved.empty() ? Path : Resolved.string();

		ELoadError Err = ELoadError::None;
		auto Loaded = LoadSidFile(LoadPath, Err);
		if (!Loaded)
		{
			const char* Msg = "Unknown load error";
			switch (Err)
			{
				case ELoadError::FileNotFound:         Msg = "File not found"; break;
				case ELoadError::FileTooShort:         Msg = "File too short to be a SID tune"; break;
				case ELoadError::BadMagic:             Msg = "Not a SID file (bad magic — expected PSID/RSID)"; break;
				case ELoadError::UnsupportedVersion:   Msg = "Unsupported PSID version"; break;
				case ELoadError::RsidUnsupported:      Msg = "RSID v3 not supported (PSID v1/v2 only in v1)"; break;
				case ELoadError::MultiSidUnsupported:  Msg = "Multi-SID tunes not supported (v1 limitation)"; break;
				case ELoadError::MalformedDataSegment: Msg = "Malformed data segment in .sid file"; break;
				default: break;
			}
			std::lock_guard<std::mutex> Lock(InfoMutex);
			LoadError = Msg;
			std::atomic_store_explicit(&ActiveEmulator,
				std::shared_ptr<FSidEmulator>{},
				std::memory_order_release);
			return;
		}

		const bool bNtsc = bRegionNtscOverride || IsNtscFromFlags(Loaded->Header.Flags);
		const double ChipClockHz = bNtsc ? 1022727.0 : 985248.0;

		auto NewEmu = std::make_shared<FSidEmulator>();
		NewEmu->Reset(ChipClockHz, SampleRate);
		if (!LoadAndInit(*NewEmu, *Loaded, Subtune, ChipClockHz, Err))
		{
			std::lock_guard<std::mutex> Lock(InfoMutex);
			LoadError = "Init routine failed";
			std::atomic_store_explicit(&ActiveEmulator,
				std::shared_ptr<FSidEmulator>{},
				std::memory_order_release);
			return;
		}

		// Stash tune metadata for the property-panel UI.
		{
			std::lock_guard<std::mutex> Lock(InfoMutex);
			Info.Name = Loaded->Header.Name;
			Info.Author = Loaded->Header.Author;
			Info.Released = Loaded->Header.Released;
			Info.Songs = Loaded->Header.Songs;
			Info.StartSong = Loaded->Header.StartSong;
			Info.bIsNtsc = bNtsc;
			Info.bIs8580 = Is8580FromFlags(Loaded->Header.Flags);
			Info.ChipClockHz = ChipClockHz;
		}

		std::atomic_store_explicit(&ActiveEmulator, std::move(NewEmu),
			std::memory_order_release);
	}

	bool FSidPlayer::GetVoiceGate(uint32_t Voice) const
	{
		if (Voice >= 3) { return false; }
		const uint32_t Out = VoiceBlocks[Voice].Gate;
		return LastValue[Out] >= 0.5f;
	}

	FSidPlayer::FLoadStatus FSidPlayer::GetStatus() const
	{
		FLoadStatus S;
		std::lock_guard<std::mutex> Lock(InfoMutex);
		S.bLoaded = !FilePath.empty() && LoadError.empty();
		S.ErrorMessage = LoadError;
		S.TuneName = Info.Name;
		S.Author = Info.Author;
		S.Released = Info.Released;
		S.Songs = Info.Songs;
		S.StartSong = Info.StartSong;
		S.bIsNtsc = Info.bIsNtsc;
		S.bIs8580 = Info.bIs8580;
		return S;
	}
}
