// Hidden-by-default Catch2 test that writes the bundled preset .json files
// out to disk. Tagged [.][preset-emit] so the standard `nodesynth_tests` run
// skips it; regenerate the bundled presets by selecting the tag explicitly:
//
//   ./build/Release/nodesynth_tests.exe "[preset-emit]"
//
// from the repo root. Output goes to ./presets/<category>/<name>.json. The
// resulting JSONs are committed to git — runtime preset loading reads from
// the binary's bundled `presets/` (copied in at build time), not from this
// test, so the only purpose here is reproducibility of preset content.

#include "io/PatchSerializer.h"

#include "dsp/Add.h"
#include "dsp/Adsr.h"
#include "dsp/AutoPan.h"
#include "dsp/Bitcrusher.h"
#include "dsp/Chorus.h"
#include "dsp/Clock.h"
#include "dsp/Compressor.h"
#include "dsp/Constant.h"
#include "dsp/Delay.h"
#include "dsp/Equalizer.h"
#include "dsp/Exciter.h"
#include "dsp/Flanger.h"
#include "dsp/Gain.h"
#include "dsp/HaasWidener.h"
#include "dsp/Lfo.h"
#include "dsp/Limiter.h"
#include "dsp/Meter.h"
#include "dsp/MicInput.h"
#include "dsp/MidiCC.h"
#include "dsp/Mixer.h"
#include "dsp/ModulationMatrix.h"
#include "dsp/Multiply.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"
#include "dsp/Phaser.h"
#include "dsp/Reverb.h"
#include "dsp/RingMod.h"
#include "dsp/SampleHold.h"
#include "dsp/Scale.h"
#include "dsp/Sequencer.h"
#include "dsp/StereoWidener.h"
#include "dsp/Svf.h"
#include "dsp/Tremolo.h"
#include "dsp/VoiceAllocator.h"
#include "dsp/Vocoder.h"
#include "dsp/Waveshaper.h"
#include "dsp/WavetableOscillator.h"
#include "graph/Graph.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace NodeSynth;

namespace
{
	struct FBuiltPatch
	{
		FGraphModel Model;
		// Direct pointers into the model's nodes for easy param tweaking
		// after the topology is built.
		FAdsr* Adsr = nullptr;
		FOscillator* Osc = nullptr;
		FGain* Gain = nullptr;
		FVoiceAllocator* Alloc = nullptr;
	};

	// Builds the same node graph as main.cpp::SeedDefaultPatch — keep this in
	// sync if SeedDefaultPatch's topology ever changes. Returns the model
	// plus pointers to the tweakable nodes so each preset variant can adjust
	// shape / envelope / glide without rebuilding the graph.
	FBuiltPatch BuildSeededPatch()
	{
		FBuiltPatch P;
		auto Alloc = std::make_shared<FVoiceAllocator>();
		auto Adsr = std::make_shared<FAdsr>();
		auto Osc = std::make_shared<FOscillator>();
		auto GainNode = std::make_shared<FGain>();
		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();

		GainNode->SetParamValue(FGain::Param_Gain, 0.15f);

		const FNodeId AllocId = P.Model.AddNode(Alloc, 60.0f, 240.0f);
		const FNodeId AdsrId = P.Model.AddNode(Adsr, 340.0f, 60.0f);
		const FNodeId OscId = P.Model.AddNode(Osc, 340.0f, 240.0f);
		const FNodeId GainId = P.Model.AddNode(GainNode, 620.0f, 180.0f);
		const FNodeId MeterId = P.Model.AddNode(MeterNode, 860.0f, 180.0f);
		const FNodeId OutId = P.Model.AddNode(Out, 1100.0f, 180.0f);

		P.Model.SetNodePerVoice(AdsrId, true);
		P.Model.SetNodePerVoice(OscId, true);

		P.Model.AddLink(AllocId, FVoiceAllocator::Output_Gate, AdsrId, 0);
		P.Model.AddLink(AllocId, FVoiceAllocator::Output_Frequency, OscId, FOscillator::Input_Frequency);
		P.Model.AddLink(AdsrId, 0, OscId, FOscillator::Input_Amplitude);
		P.Model.AddLink(OscId, 0, GainId, 0);
		P.Model.AddLink(GainId, 0, MeterId, 0);
		P.Model.AddLink(MeterId, 0, OutId, 0);

		P.Adsr = Adsr.get();
		P.Osc = Osc.get();
		P.Gain = GainNode.get();
		P.Alloc = Alloc.get();
		return P;
	}

	// Param indices match the Param_<Name> enums in each header. Repeating
	// them here as named locals keeps the call sites below readable.
	constexpr uint32_t Adsr_Attack = FAdsr::Param_AttackMs;
	constexpr uint32_t Adsr_Decay = FAdsr::Param_DecayMs;
	constexpr uint32_t Adsr_Sustain = FAdsr::Param_Sustain;
	constexpr uint32_t Adsr_Release = FAdsr::Param_ReleaseMs;
	constexpr uint32_t Osc_Shape = FOscillator::Param_Shape;
	constexpr uint32_t Gain_Level = FGain::Param_Gain;
	constexpr uint32_t Alloc_Glide = FVoiceAllocator::Param_Glide;

	// Oscillator shape indices: Sine=0, Saw=1, Square=2, Triangle=3, Noise=4.
	enum EShape : int { Sine = 0, Saw = 1, Square = 2, Triangle = 3, Noise = 4 };

	void EmitPreset(
		const std::filesystem::path& Root,
		const std::string& Category,
		const std::string& Name,
		const std::string& Notes,
		float Shape,
		float AttackMs,
		float DecayMs,
		float Sustain,
		float ReleaseMs,
		float MasterGain,
		float GlideMs)
	{
		FBuiltPatch P = BuildSeededPatch();
		P.Osc->SetParamValue(Osc_Shape, Shape);
		P.Adsr->SetParamValue(Adsr_Attack, AttackMs);
		P.Adsr->SetParamValue(Adsr_Decay, DecayMs);
		P.Adsr->SetParamValue(Adsr_Sustain, Sustain);
		P.Adsr->SetParamValue(Adsr_Release, ReleaseMs);
		P.Gain->SetParamValue(Gain_Level, MasterGain);
		P.Alloc->SetParamValue(Alloc_Glide, GlideMs);

		FPatchMetadata& Meta = P.Model.GetMetadata();
		Meta.Name = Name;
		Meta.Author = "NodeSynth";
		Meta.Notes = Notes;
		Meta.Bpm = 120.0f;
		Meta.SampleRateHint = 48000.0;

		const std::filesystem::path Dir = Root / Category;
		std::filesystem::create_directories(Dir);
		const std::filesystem::path Out = Dir / (Name + ".json");
		REQUIRE(SavePatch(P.Model, Out));
	}

	// Advanced presets — each builds its own polyphonic core (VoiceAllocator
	// → ADSR per-voice → Osc per-voice) feeding a unique post-mixer effect
	// chain. The implicit FVoiceMixer summing per-voice outputs to the mono
	// post chain is synthesised by Compile when a per-voice Audio output
	// feeds a mono Audio input — see CLAUDE.md "Polyphony" section.
	struct FCore
	{
		FNodeId AllocId = 0;
		FNodeId AdsrId = 0;
		FNodeId OscId = 0;
		FNodeId LastId = 0;          // Last node in the post-voice chain so far
		uint32_t LastOutPort = 0;
		FOscillator* Osc = nullptr;
		FAdsr* Adsr = nullptr;
		FVoiceAllocator* Alloc = nullptr;
	};

	// Builds the polyphonic core ending at a mono Gain (master trim) and
	// returns the pointers/ids needed to chain effects after it.
	FCore BuildPolyCore(FGraphModel& M, float MasterGain, EShape Shape,
		float AttackMs, float DecayMs, float Sustain, float ReleaseMs)
	{
		FCore C;
		auto Alloc = std::make_shared<FVoiceAllocator>();
		auto Adsr = std::make_shared<FAdsr>();
		auto Osc = std::make_shared<FOscillator>();
		auto GainNode = std::make_shared<FGain>();

		Adsr->SetParamValue(Adsr_Attack, AttackMs);
		Adsr->SetParamValue(Adsr_Decay, DecayMs);
		Adsr->SetParamValue(Adsr_Sustain, Sustain);
		Adsr->SetParamValue(Adsr_Release, ReleaseMs);
		Osc->SetParamValue(Osc_Shape, static_cast<float>(Shape));
		GainNode->SetParamValue(Gain_Level, MasterGain);

		C.AllocId = M.AddNode(Alloc, 60.0f, 240.0f);
		C.AdsrId = M.AddNode(Adsr, 340.0f, 60.0f);
		C.OscId = M.AddNode(Osc, 340.0f, 240.0f);
		const FNodeId GainId = M.AddNode(GainNode, 620.0f, 180.0f);

		M.SetNodePerVoice(C.AdsrId, true);
		M.SetNodePerVoice(C.OscId, true);

		M.AddLink(C.AllocId, FVoiceAllocator::Output_Gate, C.AdsrId, 0);
		M.AddLink(C.AllocId, FVoiceAllocator::Output_Frequency, C.OscId, FOscillator::Input_Frequency);
		M.AddLink(C.AdsrId, 0, C.OscId, FOscillator::Input_Amplitude);
		M.AddLink(C.OscId, 0, GainId, 0);

		C.LastId = GainId;
		C.LastOutPort = 0;
		C.Osc = Osc.get();
		C.Adsr = Adsr.get();
		C.Alloc = Alloc.get();
		return C;
	}

	// Caps the chain off with Meter → Output. Call once after adding all
	// effects to a core's post-voice chain.
	void TerminateChain(FGraphModel& M, FCore& C)
	{
		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const float X = 1340.0f;
		const FNodeId MeterId = M.AddNode(MeterNode, X, 180.0f);
		const FNodeId OutId = M.AddNode(Out, X + 240.0f, 180.0f);
		M.AddLink(C.LastId, C.LastOutPort, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);
	}

	void SetMeta(FGraphModel& M, const std::string& Name, const std::string& Notes)
	{
		FPatchMetadata& Meta = M.GetMetadata();
		Meta.Name = Name;
		Meta.Author = "NodeSynth";
		Meta.Notes = Notes;
		Meta.Bpm = 120.0f;
		Meta.SampleRateHint = 48000.0;
	}

	void SaveTo(FGraphModel& M, const std::filesystem::path& Root,
		const std::string& Category, const std::string& Name)
	{
		// Round-trip-validate: Compile at the standard rate to catch any
		// per-voice partition rejections. An empty snapshot means a bad
		// patch (e.g. per-voice -> mono Control link) — we want to know
		// here, not when the user opens the preset.
		const std::shared_ptr<FAudioGraph> Snap = M.Compile(48000.0);
		const FCompileError& Err = M.GetLastCompileError();
		INFO("Preset " << Name << " compile error: " << Err.Message);
		REQUIRE(Snap);
		REQUIRE(!Snap->OrderedNodes.empty());

		const std::filesystem::path Dir = Root / Category;
		std::filesystem::create_directories(Dir);
		REQUIRE(SavePatch(M, Dir / (Name + ".json")));
	}

	// --- Lush Pad: slow saw → 3-voice chorus → big reverb ----------------
	void EmitLushPad(const std::filesystem::path& Root)
	{
		FGraphModel M;
		FCore C = BuildPolyCore(M, 0.12f, Saw, 700.0f, 1200.0f, 0.85f, 2400.0f);

		auto Chorus = std::make_shared<FChorus>();
		auto Reverb = std::make_shared<FReverb>();
		Chorus->SetParamValue(FChorus::Param_Rate, 0.5f);
		Chorus->SetParamValue(FChorus::Param_Depth, 0.6f);
		Chorus->SetParamValue(FChorus::Param_Mix, 0.55f);
		Chorus->SetParamValue(FChorus::Param_Voices, 2.0f); // 3 voices
		Reverb->SetParamValue(FReverb::Param_RoomSize, 0.85f);
		Reverb->SetParamValue(FReverb::Param_Damping, 0.35f);
		Reverb->SetParamValue(FReverb::Param_Wet, 0.5f);

		const FNodeId ChorusId = M.AddNode(Chorus, 860.0f, 180.0f);
		const FNodeId ReverbId = M.AddNode(Reverb, 1100.0f, 180.0f);
		M.AddLink(C.LastId, C.LastOutPort, ChorusId, 0);
		M.AddLink(ChorusId, 0, ReverbId, 0);
		C.LastId = ReverbId;
		TerminateChain(M, C);

		SetMeta(M, "Lush Pad",
			"Saw -> 3-voice Chorus -> big Reverb. Slow attack and long release; holds chords with a deep, washy tail.");
		SaveTo(M, Root, "Pad", "Lush Pad");
	}

	// --- Wide Pad: triangle → chorus → stereo widener → reverb -----------
	void EmitWidePad(const std::filesystem::path& Root)
	{
		FGraphModel M;
		FCore C = BuildPolyCore(M, 0.14f, Triangle, 900.0f, 1500.0f, 0.85f, 2800.0f);

		auto Chorus = std::make_shared<FChorus>();
		auto Wide = std::make_shared<FStereoWidener>();
		auto Reverb = std::make_shared<FReverb>();
		Chorus->SetParamValue(FChorus::Param_Rate, 0.35f);
		Chorus->SetParamValue(FChorus::Param_Depth, 0.7f);
		Chorus->SetParamValue(FChorus::Param_Mix, 0.6f);
		Chorus->SetParamValue(FChorus::Param_Voices, 1.0f); // 2 voices
		Wide->SetParamValue(FStereoWidener::Param_Width, 1.6f);
		Reverb->SetParamValue(FReverb::Param_RoomSize, 0.78f);
		Reverb->SetParamValue(FReverb::Param_Damping, 0.5f);
		Reverb->SetParamValue(FReverb::Param_Wet, 0.45f);

		const FNodeId ChorusId = M.AddNode(Chorus, 860.0f, 180.0f);
		const FNodeId WideId = M.AddNode(Wide, 1100.0f, 180.0f);
		const FNodeId ReverbId = M.AddNode(Reverb, 1340.0f, 180.0f);
		M.AddLink(C.LastId, C.LastOutPort, ChorusId, 0);
		M.AddLink(ChorusId, 0, WideId, 0);
		M.AddLink(WideId, 0, ReverbId, 0);
		C.LastId = ReverbId;
		// Custom terminate to nudge meter further right.
		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const FNodeId MeterId = M.AddNode(MeterNode, 1580.0f, 180.0f);
		const FNodeId OutId = M.AddNode(Out, 1820.0f, 180.0f);
		M.AddLink(ReverbId, 0, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);

		SetMeta(M, "Wide Pad",
			"Triangle -> Chorus -> Stereo Widener (1.6x) -> Reverb. Massive stereo image; great under softer chord voicings.");
		SaveTo(M, Root, "Pad", "Wide Pad");
	}

	// --- Phased Lead: square → phaser → delay → reverb -------------------
	void EmitPhasedLead(const std::filesystem::path& Root)
	{
		FGraphModel M;
		FCore C = BuildPolyCore(M, 0.16f, Square, 8.0f, 220.0f, 0.7f, 320.0f);
		C.Alloc->SetParamValue(Alloc_Glide, 40.0f);

		auto Phaser = std::make_shared<FPhaser>();
		auto Delay = std::make_shared<FDelay>();
		auto Reverb = std::make_shared<FReverb>();
		Phaser->SetParamValue(FPhaser::Param_Rate, 0.4f);
		Phaser->SetParamValue(FPhaser::Param_Depth, 0.7f);
		Phaser->SetParamValue(FPhaser::Param_Feedback, 0.5f);
		Phaser->SetParamValue(FPhaser::Param_Mix, 0.5f);
		Phaser->SetParamValue(FPhaser::Param_Stages, 2.0f); // 6 stages
		Delay->SetParamValue(FDelay::Param_TimeMs, 375.0f);  // dotted-eighth at 120 bpm
		Delay->SetParamValue(FDelay::Param_Feedback, 0.4f);
		Delay->SetParamValue(FDelay::Param_Tone, 0.55f);
		Reverb->SetParamValue(FReverb::Param_RoomSize, 0.65f);
		Reverb->SetParamValue(FReverb::Param_Damping, 0.5f);
		Reverb->SetParamValue(FReverb::Param_Wet, 0.3f);

		const FNodeId PhaserId = M.AddNode(Phaser, 860.0f, 180.0f);
		const FNodeId DelayId = M.AddNode(Delay, 1100.0f, 180.0f);
		const FNodeId ReverbId = M.AddNode(Reverb, 1340.0f, 180.0f);
		M.AddLink(C.LastId, C.LastOutPort, PhaserId, 0);
		M.AddLink(PhaserId, 0, DelayId, 0);
		M.AddLink(DelayId, 0, ReverbId, 0);
		C.LastId = ReverbId;
		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const FNodeId MeterId = M.AddNode(MeterNode, 1580.0f, 180.0f);
		const FNodeId OutId = M.AddNode(Out, 1820.0f, 180.0f);
		M.AddLink(ReverbId, 0, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);

		SetMeta(M, "Phased Lead",
			"Square -> 6-stage Phaser -> dotted-eighth Delay -> Reverb. Glide gives legato runs a vocal slur.");
		SaveTo(M, Root, "Lead", "Phased Lead");
	}

	// --- Gritty Bass: saw → hard clip → compressor -----------------------
	void EmitGrittyBass(const std::filesystem::path& Root)
	{
		FGraphModel M;
		FCore C = BuildPolyCore(M, 0.18f, Saw, 1.0f, 90.0f, 0.65f, 110.0f);

		auto Shaper = std::make_shared<FWaveshaper>();
		auto Comp = std::make_shared<FCompressor>();
		Shaper->SetParamValue(FWaveshaper::Param_Shape, static_cast<float>(EWaveshape::HardClip));
		Shaper->SetParamValue(FWaveshaper::Param_DriveDb, 9.0f);
		Shaper->SetParamValue(FWaveshaper::Param_OutputDb, -6.0f);
		Shaper->SetParamValue(FWaveshaper::Param_Oversample, 1.0f); // 2x
		Comp->SetParamValue(FCompressor::Param_ThresholdDb, -18.0f);
		Comp->SetParamValue(FCompressor::Param_Ratio, 4.0f);
		Comp->SetParamValue(FCompressor::Param_AttackMs, 5.0f);
		Comp->SetParamValue(FCompressor::Param_ReleaseMs, 80.0f);
		Comp->SetParamValue(FCompressor::Param_MakeupGainDb, 4.0f);

		const FNodeId ShaperId = M.AddNode(Shaper, 860.0f, 180.0f);
		const FNodeId CompId = M.AddNode(Comp, 1100.0f, 180.0f);
		M.AddLink(C.LastId, C.LastOutPort, ShaperId, 0);
		M.AddLink(ShaperId, 0, CompId, 0);
		C.LastId = CompId;
		TerminateChain(M, C);

		SetMeta(M, "Gritty Bass",
			"Saw -> HardClip Waveshaper (2x oversample) -> Compressor. Snappy envelope, tight low end.");
		SaveTo(M, Root, "Bass", "Gritty Bass");
	}

	// --- Bell Tone: triangle → ring mod → reverb -------------------------
	void EmitBellTone(const std::filesystem::path& Root)
	{
		FGraphModel M;
		FCore C = BuildPolyCore(M, 0.14f, Triangle, 2.0f, 600.0f, 0.0f, 600.0f);

		auto Ring = std::make_shared<FRingMod>();
		auto Reverb = std::make_shared<FReverb>();
		Ring->SetParamValue(FRingMod::Param_CarrierHz, 220.0f);
		Ring->SetParamValue(FRingMod::Param_Shape, 0.0f); // Sine
		Ring->SetParamValue(FRingMod::Param_Mix, 0.7f);
		Reverb->SetParamValue(FReverb::Param_RoomSize, 0.75f);
		Reverb->SetParamValue(FReverb::Param_Damping, 0.4f);
		Reverb->SetParamValue(FReverb::Param_Wet, 0.45f);

		const FNodeId RingId = M.AddNode(Ring, 860.0f, 180.0f);
		const FNodeId ReverbId = M.AddNode(Reverb, 1100.0f, 180.0f);
		M.AddLink(C.LastId, C.LastOutPort, RingId, 0);
		M.AddLink(RingId, 0, ReverbId, 0);
		C.LastId = ReverbId;
		TerminateChain(M, C);

		SetMeta(M, "Bell Tone",
			"Triangle -> Ring Modulator (220 Hz sine carrier) -> Reverb. Decay-only envelope; metallic, plucky bells. Inharmonic with held notes.");
		SaveTo(M, Root, "FX", "Bell Tone");
	}

	// --- Crushed Stab: saw → bitcrusher → delay --------------------------
	void EmitCrushedStab(const std::filesystem::path& Root)
	{
		FGraphModel M;
		FCore C = BuildPolyCore(M, 0.16f, Saw, 2.0f, 140.0f, 0.5f, 180.0f);

		auto Crush = std::make_shared<FBitcrusher>();
		auto Delay = std::make_shared<FDelay>();
		Crush->SetParamValue(FBitcrusher::Param_SampleRateRatio, 0.18f);
		Crush->SetParamValue(FBitcrusher::Param_Bits, 6.0f);
		Crush->SetParamValue(FBitcrusher::Param_Mix, 0.85f);
		Delay->SetParamValue(FDelay::Param_TimeMs, 250.0f);   // eighth at 120 bpm
		Delay->SetParamValue(FDelay::Param_Feedback, 0.5f);
		Delay->SetParamValue(FDelay::Param_Tone, 0.4f);

		const FNodeId CrushId = M.AddNode(Crush, 860.0f, 180.0f);
		const FNodeId DelayId = M.AddNode(Delay, 1100.0f, 180.0f);
		M.AddLink(C.LastId, C.LastOutPort, CrushId, 0);
		M.AddLink(CrushId, 0, DelayId, 0);
		C.LastId = DelayId;
		TerminateChain(M, C);

		SetMeta(M, "Crushed Stab",
			"Saw -> Bitcrusher (6-bit, ~9 kHz) -> eighth-note Delay. Lo-fi character with rhythmic echo tails.");
		SaveTo(M, Root, "FX", "Crushed Stab");
	}

	// --- Dub Stab: square → long delay → reverb --------------------------
	void EmitDubStab(const std::filesystem::path& Root)
	{
		FGraphModel M;
		FCore C = BuildPolyCore(M, 0.16f, Square, 1.0f, 120.0f, 0.0f, 100.0f);

		auto Delay = std::make_shared<FDelay>();
		auto Reverb = std::make_shared<FReverb>();
		Delay->SetParamValue(FDelay::Param_TimeMs, 500.0f);  // quarter at 120 bpm
		Delay->SetParamValue(FDelay::Param_Feedback, 0.65f);
		Delay->SetParamValue(FDelay::Param_Tone, 0.25f);     // dark
		Reverb->SetParamValue(FReverb::Param_RoomSize, 0.7f);
		Reverb->SetParamValue(FReverb::Param_Damping, 0.6f);
		Reverb->SetParamValue(FReverb::Param_Wet, 0.35f);

		const FNodeId DelayId = M.AddNode(Delay, 860.0f, 180.0f);
		const FNodeId ReverbId = M.AddNode(Reverb, 1100.0f, 180.0f);
		M.AddLink(C.LastId, C.LastOutPort, DelayId, 0);
		M.AddLink(DelayId, 0, ReverbId, 0);
		C.LastId = ReverbId;
		TerminateChain(M, C);

		SetMeta(M, "Dub Stab",
			"Square -> long dark Delay (quarter-note feedback) -> Reverb. Decay-only envelope so each stab triggers a fresh dub echo trail.");
		SaveTo(M, Root, "FX", "Dub Stab");
	}

	// --- Tremolo Lead: saw → tremolo → exciter ---------------------------
	void EmitTremoloLead(const std::filesystem::path& Root)
	{
		FGraphModel M;
		FCore C = BuildPolyCore(M, 0.16f, Saw, 6.0f, 200.0f, 0.7f, 280.0f);

		auto Trem = std::make_shared<FTremolo>();
		auto Exc = std::make_shared<FExciter>();
		Trem->SetParamValue(FTremolo::Param_Rate, 5.5f);
		Trem->SetParamValue(FTremolo::Param_Depth, 0.6f);
		Trem->SetParamValue(FTremolo::Param_Shape, 0.0f);   // Sine
		Trem->SetParamValue(FTremolo::Param_Stereo, 1.0f);  // Quad (180°)
		Exc->SetParamValue(FExciter::Param_Frequency, 4000.0f);
		Exc->SetParamValue(FExciter::Param_DriveDb, 14.0f);
		Exc->SetParamValue(FExciter::Param_Mix, 0.35f);

		const FNodeId TremId = M.AddNode(Trem, 860.0f, 180.0f);
		const FNodeId ExcId = M.AddNode(Exc, 1100.0f, 180.0f);
		M.AddLink(C.LastId, C.LastOutPort, TremId, 0);
		M.AddLink(TremId, 0, ExcId, 0);
		C.LastId = ExcId;
		TerminateChain(M, C);

		SetMeta(M, "Tremolo Lead",
			"Saw -> stereo Tremolo (~5.5 Hz, 180° L/R offset) -> Exciter. Pulsing width with extra air on top.");
		SaveTo(M, Root, "Lead", "Tremolo Lead");
	}

	// --- Filter Sweep Lead: ADSR-modulated SVF cutoff (per-voice) -------
	// Demonstrates routing the per-voice envelope into the per-voice filter
	// cutoff as well as the oscillator amplitude — a single envelope shapes
	// both timbre and loudness.
	void EmitFilterSweepLead(const std::filesystem::path& Root)
	{
		FGraphModel M;
		auto Alloc = std::make_shared<FVoiceAllocator>();
		auto Adsr = std::make_shared<FAdsr>();
		auto Osc = std::make_shared<FOscillator>();
		auto Scale = std::make_shared<FScale>();
		auto Filt = std::make_shared<FSvf>();
		auto GainNode = std::make_shared<FGain>();

		Adsr->SetParamValue(FAdsr::Param_AttackMs, 4.0f);
		Adsr->SetParamValue(FAdsr::Param_DecayMs, 600.0f);
		Adsr->SetParamValue(FAdsr::Param_Sustain, 0.35f);
		Adsr->SetParamValue(FAdsr::Param_ReleaseMs, 350.0f);
		Osc->SetParamValue(FOscillator::Param_Shape, static_cast<float>(Saw));
		Scale->SetParamValue(FScale::Param_InMin, 0.0f);
		Scale->SetParamValue(FScale::Param_InMax, 1.0f);
		Scale->SetParamValue(FScale::Param_OutMin, 250.0f);
		Scale->SetParamValue(FScale::Param_OutMax, 4500.0f);
		Filt->SetParamValue(FSvf::Param_Resonance, 0.78f);
		GainNode->SetParamValue(FGain::Param_Gain, 0.16f);

		const FNodeId AllocId = M.AddNode(Alloc, 60.0f, 240.0f);
		const FNodeId AdsrId = M.AddNode(Adsr, 320.0f, 60.0f);
		const FNodeId OscId = M.AddNode(Osc, 320.0f, 240.0f);
		const FNodeId ScaleId = M.AddNode(Scale, 580.0f, 60.0f);
		const FNodeId FiltId = M.AddNode(Filt, 580.0f, 240.0f);
		const FNodeId GainId = M.AddNode(GainNode, 840.0f, 240.0f);

		M.SetNodePerVoice(AdsrId, true);
		M.SetNodePerVoice(OscId, true);
		M.SetNodePerVoice(ScaleId, true);
		M.SetNodePerVoice(FiltId, true);

		M.AddLink(AllocId, FVoiceAllocator::Output_Gate, AdsrId, 0);
		M.AddLink(AllocId, FVoiceAllocator::Output_Frequency, OscId, FOscillator::Input_Frequency);
		M.AddLink(AdsrId, 0, OscId, FOscillator::Input_Amplitude);
		M.AddLink(AdsrId, 0, ScaleId, 0);                  // Scale.In
		M.AddLink(ScaleId, 0, FiltId, FSvf::Input_Cutoff);
		M.AddLink(OscId, 0, FiltId, FSvf::Input_Audio);
		M.AddLink(FiltId, FSvf::Output_LowPass, GainId, 0);

		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const FNodeId MeterId = M.AddNode(MeterNode, 1080.0f, 240.0f);
		const FNodeId OutId = M.AddNode(Out, 1320.0f, 240.0f);
		M.AddLink(GainId, 0, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);

		SetMeta(M, "Filter Sweep Lead",
			"Saw -> per-voice SVF (resonant LP). The ADSR drives both osc amplitude and SVF cutoff (250-4500 Hz via Scale), so each note opens then closes the filter as it decays.");
		SaveTo(M, Root, "Lead", "Filter Sweep Lead");
	}

	// --- Wobble Bass: LFO-modulated SVF cutoff with saturation ----------
	// Shows mono->per-voice broadcast: the single LFO drives every voice's
	// SVF cutoff in lockstep, then the summed output runs through a tanh
	// waveshaper and a compressor for that snarling sub-bass character.
	void EmitWobbleBass(const std::filesystem::path& Root)
	{
		FGraphModel M;
		auto Alloc = std::make_shared<FVoiceAllocator>();
		auto Adsr = std::make_shared<FAdsr>();
		auto Osc = std::make_shared<FOscillator>();
		auto Filt = std::make_shared<FSvf>();
		auto Lfo = std::make_shared<FLfo>();
		auto LfoScale = std::make_shared<FScale>();
		auto Shaper = std::make_shared<FWaveshaper>();
		auto Comp = std::make_shared<FCompressor>();
		auto GainNode = std::make_shared<FGain>();

		Adsr->SetParamValue(FAdsr::Param_AttackMs, 2.0f);
		Adsr->SetParamValue(FAdsr::Param_DecayMs, 80.0f);
		Adsr->SetParamValue(FAdsr::Param_Sustain, 0.85f);
		Adsr->SetParamValue(FAdsr::Param_ReleaseMs, 120.0f);
		Osc->SetParamValue(FOscillator::Param_Shape, static_cast<float>(Saw));
		Filt->SetParamValue(FSvf::Param_Resonance, 0.7f);
		Lfo->SetParamValue(FLfo::Param_Shape, 1.0f);   // Triangle
		Lfo->SetParamValue(FLfo::Param_RateHz, 2.0f);
		Lfo->SetParamValue(FLfo::Param_Amount, 1.0f);
		LfoScale->SetParamValue(FScale::Param_InMin, -1.0f);
		LfoScale->SetParamValue(FScale::Param_InMax, 1.0f);
		LfoScale->SetParamValue(FScale::Param_OutMin, 120.0f);
		LfoScale->SetParamValue(FScale::Param_OutMax, 1800.0f);
		Shaper->SetParamValue(FWaveshaper::Param_Shape, static_cast<float>(EWaveshape::TanhSoft));
		Shaper->SetParamValue(FWaveshaper::Param_DriveDb, 7.0f);
		Shaper->SetParamValue(FWaveshaper::Param_OutputDb, -3.0f);
		Comp->SetParamValue(FCompressor::Param_ThresholdDb, -16.0f);
		Comp->SetParamValue(FCompressor::Param_Ratio, 4.0f);
		Comp->SetParamValue(FCompressor::Param_AttackMs, 5.0f);
		Comp->SetParamValue(FCompressor::Param_ReleaseMs, 80.0f);
		Comp->SetParamValue(FCompressor::Param_MakeupGainDb, 3.0f);
		GainNode->SetParamValue(FGain::Param_Gain, 0.18f);

		const FNodeId AllocId = M.AddNode(Alloc, 60.0f, 280.0f);
		const FNodeId AdsrId = M.AddNode(Adsr, 320.0f, 100.0f);
		const FNodeId OscId = M.AddNode(Osc, 320.0f, 280.0f);
		const FNodeId LfoId = M.AddNode(Lfo, 60.0f, 460.0f);
		const FNodeId LfoScaleId = M.AddNode(LfoScale, 320.0f, 460.0f);
		const FNodeId FiltId = M.AddNode(Filt, 580.0f, 280.0f);
		const FNodeId ShaperId = M.AddNode(Shaper, 840.0f, 280.0f);
		const FNodeId CompId = M.AddNode(Comp, 1080.0f, 280.0f);
		const FNodeId GainId = M.AddNode(GainNode, 1320.0f, 280.0f);

		M.SetNodePerVoice(AdsrId, true);
		M.SetNodePerVoice(OscId, true);
		M.SetNodePerVoice(FiltId, true);

		M.AddLink(AllocId, FVoiceAllocator::Output_Gate, AdsrId, 0);
		M.AddLink(AllocId, FVoiceAllocator::Output_Frequency, OscId, FOscillator::Input_Frequency);
		M.AddLink(AdsrId, 0, OscId, FOscillator::Input_Amplitude);
		M.AddLink(OscId, 0, FiltId, FSvf::Input_Audio);
		M.AddLink(LfoId, 0, LfoScaleId, 0);
		M.AddLink(LfoScaleId, 0, FiltId, FSvf::Input_Cutoff); // mono -> per-voice broadcast
		M.AddLink(FiltId, FSvf::Output_LowPass, ShaperId, 0);
		M.AddLink(ShaperId, 0, CompId, 0);
		M.AddLink(CompId, 0, GainId, 0);

		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const FNodeId MeterId = M.AddNode(MeterNode, 1560.0f, 280.0f);
		const FNodeId OutId = M.AddNode(Out, 1800.0f, 280.0f);
		M.AddLink(GainId, 0, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);

		SetMeta(M, "Wobble Bass",
			"Saw -> per-voice resonant SVF, with a 2 Hz LFO sweeping cutoff (120-1800 Hz). The wobble runs through tanh saturation and a 4:1 compressor for the dubstep snarl.");
		SaveTo(M, Root, "Bass", "Wobble Bass");
	}

	// --- Acid Sequence: self-playing 16-step sequencer ------------------
	// No voice allocator and no MIDI input — the Clock drives a Sequencer
	// which drives an Oscillator + ADSR + filter chain. Plays itself the
	// instant the patch loads. Demonstrates Clock + Sequencer + ADSR-routed
	// filter modulation in a fully mono path.
	void EmitAcidSequence(const std::filesystem::path& Root)
	{
		FGraphModel M;
		auto Clock = std::make_shared<FClock>();
		auto Seq = std::make_shared<FSequencer>();
		auto Adsr = std::make_shared<FAdsr>();
		auto Osc = std::make_shared<FOscillator>();
		auto Scale = std::make_shared<FScale>();
		auto Filt = std::make_shared<FSvf>();
		auto Delay = std::make_shared<FDelay>();
		auto Reverb = std::make_shared<FReverb>();
		auto GainNode = std::make_shared<FGain>();

		Clock->SetParamValue(FClock::Param_Bpm, 130.0f);
		Seq->SetParamValue(FSequencer::Param_NumSteps, 16.0f);
		Seq->SetParamValue(FSequencer::Param_RootNote, 36.0f); // C2

		// 16-step acid riff in semitones from RootNote (C2). Mix of unison,
		// octaves, fifths, and a low fifth — classic 303-style movement.
		const float Pitches[16] = {
			0,  0,  12,  0,   7,  0,  0, 12,
			0,  0,   5,  0,  12,  0,  7,  0,
		};
		// All steps enabled, full velocity, gate length ~50 % so each note
		// closes before the next step begins.
		for (int32_t I = 0; I < 16; ++I)
		{
			Seq->SetParamValue(FSequencer::Param_StepPitchBase + I, Pitches[I]);
			Seq->SetParamValue(FSequencer::Param_StepVelocityBase + I, 1.0f);
			Seq->SetParamValue(FSequencer::Param_StepGateLengthBase + I, 0.5f);
			Seq->SetParamValue(FSequencer::Param_StepEnabledBase + I, 1.0f);
		}

		Adsr->SetParamValue(FAdsr::Param_AttackMs, 1.0f);
		Adsr->SetParamValue(FAdsr::Param_DecayMs, 180.0f);
		Adsr->SetParamValue(FAdsr::Param_Sustain, 0.0f);    // pluck
		Adsr->SetParamValue(FAdsr::Param_ReleaseMs, 80.0f);
		Osc->SetParamValue(FOscillator::Param_Shape, static_cast<float>(Saw));
		Scale->SetParamValue(FScale::Param_InMin, 0.0f);
		Scale->SetParamValue(FScale::Param_InMax, 1.0f);
		Scale->SetParamValue(FScale::Param_OutMin, 200.0f);
		Scale->SetParamValue(FScale::Param_OutMax, 3500.0f);
		Filt->SetParamValue(FSvf::Param_Resonance, 0.85f);  // squelch
		Delay->SetParamValue(FDelay::Param_TimeMs, 230.0f);  // ~ eighth at 130 BPM
		Delay->SetParamValue(FDelay::Param_Feedback, 0.4f);
		Delay->SetParamValue(FDelay::Param_Tone, 0.5f);
		Reverb->SetParamValue(FReverb::Param_RoomSize, 0.55f);
		Reverb->SetParamValue(FReverb::Param_Damping, 0.5f);
		Reverb->SetParamValue(FReverb::Param_Wet, 0.25f);
		GainNode->SetParamValue(FGain::Param_Gain, 0.35f);

		const FNodeId ClockId = M.AddNode(Clock, 60.0f, 100.0f);
		const FNodeId SeqId = M.AddNode(Seq, 320.0f, 100.0f);
		const FNodeId AdsrId = M.AddNode(Adsr, 580.0f, 60.0f);
		const FNodeId OscId = M.AddNode(Osc, 580.0f, 240.0f);
		const FNodeId ScaleId = M.AddNode(Scale, 840.0f, 60.0f);
		const FNodeId FiltId = M.AddNode(Filt, 840.0f, 240.0f);
		const FNodeId GainId = M.AddNode(GainNode, 1080.0f, 240.0f);
		const FNodeId DelayId = M.AddNode(Delay, 1320.0f, 240.0f);
		const FNodeId ReverbId = M.AddNode(Reverb, 1560.0f, 240.0f);

		M.AddLink(ClockId, 0, SeqId, FSequencer::Input_Clock);
		M.AddLink(SeqId, FSequencer::Output_Gate, AdsrId, 0);
		M.AddLink(SeqId, FSequencer::Output_Frequency, OscId, FOscillator::Input_Frequency);
		M.AddLink(AdsrId, 0, OscId, FOscillator::Input_Amplitude);
		M.AddLink(AdsrId, 0, ScaleId, 0);
		M.AddLink(ScaleId, 0, FiltId, FSvf::Input_Cutoff);
		M.AddLink(OscId, 0, FiltId, FSvf::Input_Audio);
		M.AddLink(FiltId, FSvf::Output_LowPass, GainId, 0);
		M.AddLink(GainId, 0, DelayId, 0);
		M.AddLink(DelayId, 0, ReverbId, 0);

		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const FNodeId MeterId = M.AddNode(MeterNode, 1800.0f, 240.0f);
		const FNodeId OutId = M.AddNode(Out, 2040.0f, 240.0f);
		M.AddLink(ReverbId, 0, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);

		SetMeta(M, "Acid Sequence",
			"Self-playing. Clock (130 BPM) -> 16-step Sequencer -> Saw -> resonant filter (envelope-modulated) -> Delay -> Reverb. Loads and plays itself; press a key to retrigger nothing — this patch ignores the keyboard.");
		SaveTo(M, Root, "FX", "Acid Sequence");
	}

	// --- Mastered Pad: full EQ -> Compressor -> Limiter chain -----------
	// Showcase the dynamics + spectral shaping nodes from Stage E.1/E.2 in
	// a polished pad context. EQ scoops some low-mid, lifts air; compressor
	// glues the polyphonic stack; limiter catches transients before output.
	void EmitMasteredPad(const std::filesystem::path& Root)
	{
		FGraphModel M;
		FCore C = BuildPolyCore(M, 0.20f, Saw, 800.0f, 1500.0f, 0.85f, 2400.0f);

		auto Eq = std::make_shared<FEqualizer>();
		auto Comp = std::make_shared<FCompressor>();
		auto Limit = std::make_shared<FLimiter>();
		Eq->SetParamValue(FEqualizer::Param_LowShelfFreq, 220.0f);
		Eq->SetParamValue(FEqualizer::Param_LowShelfGainDb, -2.0f);
		Eq->SetParamValue(FEqualizer::Param_PeakFreq, 600.0f);
		Eq->SetParamValue(FEqualizer::Param_PeakGainDb, -3.0f);   // scoop boxy mid
		Eq->SetParamValue(FEqualizer::Param_PeakQ, 1.2f);
		Eq->SetParamValue(FEqualizer::Param_HighShelfFreq, 6000.0f);
		Eq->SetParamValue(FEqualizer::Param_HighShelfGainDb, 4.0f); // air
		Comp->SetParamValue(FCompressor::Param_ThresholdDb, -20.0f);
		Comp->SetParamValue(FCompressor::Param_Ratio, 3.0f);
		Comp->SetParamValue(FCompressor::Param_AttackMs, 15.0f);
		Comp->SetParamValue(FCompressor::Param_ReleaseMs, 200.0f);
		Comp->SetParamValue(FCompressor::Param_MakeupGainDb, 4.0f);
		Limit->SetParamValue(FLimiter::Param_CeilingDb, -1.0f);
		Limit->SetParamValue(FLimiter::Param_ReleaseMs, 80.0f);
		Limit->SetParamValue(FLimiter::Param_MakeupGainDb, 0.0f);

		const FNodeId EqId = M.AddNode(Eq, 860.0f, 180.0f);
		const FNodeId CompId = M.AddNode(Comp, 1100.0f, 180.0f);
		const FNodeId LimitId = M.AddNode(Limit, 1340.0f, 180.0f);
		M.AddLink(C.LastId, C.LastOutPort, EqId, 0);
		M.AddLink(EqId, 0, CompId, 0);
		M.AddLink(CompId, 0, LimitId, 0);
		C.LastId = LimitId;

		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const FNodeId MeterId = M.AddNode(MeterNode, 1580.0f, 180.0f);
		const FNodeId OutId = M.AddNode(Out, 1820.0f, 180.0f);
		M.AddLink(LimitId, 0, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);

		SetMeta(M, "Mastered Pad",
			"Saw pad -> 3-band EQ (low cut, mid scoop, air boost) -> 3:1 Compressor -> Limiter. Demonstrates a full mastering chain glued onto a polyphonic source.");
		SaveTo(M, Root, "Pad", "Mastered Pad");
	}

	// --- Stepped LFO Bass: clocked Sample-and-Hold modulating the filter
	// Demonstrates SampleHold + Clock + LFO + Scale as a stepped modulation
	// generator. Each clock tick latches the current LFO value, producing a
	// stair-stepped filter pattern that locks to tempo.
	void EmitSteppedLfoBass(const std::filesystem::path& Root)
	{
		FGraphModel M;
		auto Alloc = std::make_shared<FVoiceAllocator>();
		auto Adsr = std::make_shared<FAdsr>();
		auto Osc = std::make_shared<FOscillator>();
		auto Filt = std::make_shared<FSvf>();
		auto Clock = std::make_shared<FClock>();
		auto Lfo = std::make_shared<FLfo>();
		auto SH = std::make_shared<FSampleHold>();
		auto SHScale = std::make_shared<FScale>();
		auto GainNode = std::make_shared<FGain>();

		Adsr->SetParamValue(FAdsr::Param_AttackMs, 2.0f);
		Adsr->SetParamValue(FAdsr::Param_DecayMs, 100.0f);
		Adsr->SetParamValue(FAdsr::Param_Sustain, 0.7f);
		Adsr->SetParamValue(FAdsr::Param_ReleaseMs, 120.0f);
		Osc->SetParamValue(FOscillator::Param_Shape, static_cast<float>(Saw));
		Filt->SetParamValue(FSvf::Param_Resonance, 0.6f);
		Clock->SetParamValue(FClock::Param_Bpm, 240.0f);  // fast — eighth-note steps at 120 BPM
		Lfo->SetParamValue(FLfo::Param_Shape, 0.0f);      // Sine
		Lfo->SetParamValue(FLfo::Param_RateHz, 0.7f);     // slow envelope of the steps
		Lfo->SetParamValue(FLfo::Param_Amount, 1.0f);
		SHScale->SetParamValue(FScale::Param_InMin, -1.0f);
		SHScale->SetParamValue(FScale::Param_InMax, 1.0f);
		SHScale->SetParamValue(FScale::Param_OutMin, 200.0f);
		SHScale->SetParamValue(FScale::Param_OutMax, 2400.0f);
		GainNode->SetParamValue(FGain::Param_Gain, 0.18f);

		const FNodeId AllocId = M.AddNode(Alloc, 60.0f, 280.0f);
		const FNodeId AdsrId = M.AddNode(Adsr, 320.0f, 100.0f);
		const FNodeId OscId = M.AddNode(Osc, 320.0f, 280.0f);
		const FNodeId ClockId = M.AddNode(Clock, 60.0f, 460.0f);
		const FNodeId LfoId = M.AddNode(Lfo, 60.0f, 580.0f);
		const FNodeId SHId = M.AddNode(SH, 320.0f, 460.0f);
		const FNodeId SHScaleId = M.AddNode(SHScale, 580.0f, 460.0f);
		const FNodeId FiltId = M.AddNode(Filt, 580.0f, 280.0f);
		const FNodeId GainId = M.AddNode(GainNode, 840.0f, 280.0f);

		M.SetNodePerVoice(AdsrId, true);
		M.SetNodePerVoice(OscId, true);
		M.SetNodePerVoice(FiltId, true);

		M.AddLink(AllocId, FVoiceAllocator::Output_Gate, AdsrId, 0);
		M.AddLink(AllocId, FVoiceAllocator::Output_Frequency, OscId, FOscillator::Input_Frequency);
		M.AddLink(AdsrId, 0, OscId, FOscillator::Input_Amplitude);
		M.AddLink(OscId, 0, FiltId, FSvf::Input_Audio);
		M.AddLink(LfoId, 0, SHId, FSampleHold::Input_In);
		M.AddLink(ClockId, 0, SHId, FSampleHold::Input_Trigger);
		M.AddLink(SHId, 0, SHScaleId, 0);
		M.AddLink(SHScaleId, 0, FiltId, FSvf::Input_Cutoff);
		M.AddLink(FiltId, FSvf::Output_LowPass, GainId, 0);

		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const FNodeId MeterId = M.AddNode(MeterNode, 1080.0f, 280.0f);
		const FNodeId OutId = M.AddNode(Out, 1320.0f, 280.0f);
		M.AddLink(GainId, 0, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);

		SetMeta(M, "Stepped LFO Bass",
			"Saw -> per-voice resonant filter, where a slow LFO is sampled-and-held by a 240-BPM clock to drive cutoff in tempo-locked steps. Try holding a chord — every voice's filter steps in unison.");
		SaveTo(M, Root, "Bass", "Stepped LFO Bass");
	}

	// --- Super Saw: 3 detuned per-voice oscillators through a Mixer -----
	// Uses the Math nodes + Mixer + per-voice flag to build a classic
	// supersaw: 3 saw oscillators per voice, two of them detuned via
	// Multiply * Constant on the allocator's frequency, summed by a
	// per-voice Mixer, then sweetened with a chorus.
	void EmitSuperSaw(const std::filesystem::path& Root)
	{
		FGraphModel M;
		auto Alloc = std::make_shared<FVoiceAllocator>();
		auto Adsr = std::make_shared<FAdsr>();
		auto Osc1 = std::make_shared<FOscillator>();
		auto Osc2 = std::make_shared<FOscillator>();
		auto Osc3 = std::make_shared<FOscillator>();
		auto DetuneUp = std::make_shared<FConstant>();
		auto DetuneDown = std::make_shared<FConstant>();
		auto MulUp = std::make_shared<FMultiply>();
		auto MulDown = std::make_shared<FMultiply>();
		auto Mix = std::make_shared<FMixer>();
		auto Filt = std::make_shared<FSvf>();
		auto Chor = std::make_shared<FChorus>();
		auto GainNode = std::make_shared<FGain>();

		Adsr->SetParamValue(FAdsr::Param_AttackMs, 12.0f);
		Adsr->SetParamValue(FAdsr::Param_DecayMs, 250.0f);
		Adsr->SetParamValue(FAdsr::Param_Sustain, 0.8f);
		Adsr->SetParamValue(FAdsr::Param_ReleaseMs, 400.0f);
		Osc1->SetParamValue(FOscillator::Param_Shape, static_cast<float>(Saw));
		Osc2->SetParamValue(FOscillator::Param_Shape, static_cast<float>(Saw));
		Osc3->SetParamValue(FOscillator::Param_Shape, static_cast<float>(Saw));
		// ~12 cents detune up / down (2^(12/1200) ~ 1.00696, 2^(-12/1200) ~ 0.99309).
		DetuneUp->SetParamValue(FConstant::Param_Value, 1.007f);
		DetuneDown->SetParamValue(FConstant::Param_Value, 0.993f);
		Mix->SetParamValue(FMixer::Param_Gain1, 0.5f);
		Mix->SetParamValue(FMixer::Param_Gain2, 0.5f);
		Mix->SetParamValue(FMixer::Param_Gain3, 0.5f);
		Mix->SetParamValue(FMixer::Param_Gain4, 0.0f);
		Filt->SetParamValue(FSvf::Param_Cutoff, 4500.0f);
		Filt->SetParamValue(FSvf::Param_Resonance, 0.25f);
		Chor->SetParamValue(FChorus::Param_Rate, 0.45f);
		Chor->SetParamValue(FChorus::Param_Depth, 0.55f);
		Chor->SetParamValue(FChorus::Param_Mix, 0.5f);
		Chor->SetParamValue(FChorus::Param_Voices, 1.0f);  // 2-voice
		GainNode->SetParamValue(FGain::Param_Gain, 0.10f);  // 3 oscs × 8 voices needs head-room

		const FNodeId AllocId = M.AddNode(Alloc, 60.0f, 320.0f);
		const FNodeId AdsrId = M.AddNode(Adsr, 320.0f, 60.0f);
		const FNodeId DetUpId = M.AddNode(DetuneUp, 60.0f, 460.0f);
		const FNodeId DetDownId = M.AddNode(DetuneDown, 60.0f, 580.0f);
		const FNodeId MulUpId = M.AddNode(MulUp, 320.0f, 460.0f);
		const FNodeId MulDownId = M.AddNode(MulDown, 320.0f, 580.0f);
		const FNodeId Osc1Id = M.AddNode(Osc1, 580.0f, 200.0f);
		const FNodeId Osc2Id = M.AddNode(Osc2, 580.0f, 360.0f);
		const FNodeId Osc3Id = M.AddNode(Osc3, 580.0f, 520.0f);
		const FNodeId MixId = M.AddNode(Mix, 840.0f, 360.0f);
		const FNodeId FiltId = M.AddNode(Filt, 1080.0f, 360.0f);
		const FNodeId ChorId = M.AddNode(Chor, 1320.0f, 360.0f);
		const FNodeId GainId = M.AddNode(GainNode, 1560.0f, 360.0f);

		// Per-voice flags: ADSR, all oscillators, both multipliers, mixer.
		// Constants stay mono (broadcast their value to each clone's MulX.B).
		M.SetNodePerVoice(AdsrId, true);
		M.SetNodePerVoice(MulUpId, true);
		M.SetNodePerVoice(MulDownId, true);
		M.SetNodePerVoice(Osc1Id, true);
		M.SetNodePerVoice(Osc2Id, true);
		M.SetNodePerVoice(Osc3Id, true);
		M.SetNodePerVoice(MixId, true);

		// Allocator -> ADSR (Gate) and -> Osc1 directly. Osc2 / Osc3 receive
		// detuned frequencies (Allocator.Freq * DetuneUp / DetuneDown).
		M.AddLink(AllocId, FVoiceAllocator::Output_Gate, AdsrId, 0);
		M.AddLink(AllocId, FVoiceAllocator::Output_Frequency, Osc1Id, FOscillator::Input_Frequency);
		M.AddLink(AllocId, FVoiceAllocator::Output_Frequency, MulUpId, 0);
		M.AddLink(DetUpId, 0, MulUpId, 1);
		M.AddLink(MulUpId, 0, Osc2Id, FOscillator::Input_Frequency);
		M.AddLink(AllocId, FVoiceAllocator::Output_Frequency, MulDownId, 0);
		M.AddLink(DetDownId, 0, MulDownId, 1);
		M.AddLink(MulDownId, 0, Osc3Id, FOscillator::Input_Frequency);

		// Per-voice ADSR shapes each oscillator's amplitude.
		M.AddLink(AdsrId, 0, Osc1Id, FOscillator::Input_Amplitude);
		M.AddLink(AdsrId, 0, Osc2Id, FOscillator::Input_Amplitude);
		M.AddLink(AdsrId, 0, Osc3Id, FOscillator::Input_Amplitude);

		// Per-voice Mixer sums the three oscillators; per-voice -> mono Audio
		// at MixId -> FiltId triggers the synthesised voice mixer.
		M.AddLink(Osc1Id, 0, MixId, 0);
		M.AddLink(Osc2Id, 0, MixId, 1);
		M.AddLink(Osc3Id, 0, MixId, 2);

		M.AddLink(MixId, 0, FiltId, FSvf::Input_Audio);
		M.AddLink(FiltId, FSvf::Output_LowPass, ChorId, 0);
		M.AddLink(ChorId, 0, GainId, 0);

		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const FNodeId MeterId = M.AddNode(MeterNode, 1800.0f, 360.0f);
		const FNodeId OutId = M.AddNode(Out, 2040.0f, 360.0f);
		M.AddLink(GainId, 0, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);

		SetMeta(M, "Super Saw",
			"Three saw oscillators per voice, two of them detuned ~12 cents up/down via Multiply x Constant on the allocator frequency. Per-voice Mixer sums them, then SVF -> Chorus.");
		SaveTo(M, Root, "Lead", "Super Saw");
	}

	// =============================================================
	// Modulation Matrix showcase — two LFOs feed two filter targets
	// through one matrix node, replacing the LFO×Scale×Add chain.
	// =============================================================

	void EmitMatrixRoutingPad(const std::filesystem::path& Root)
	{
		FGraphModel M;
		auto Alloc = std::make_shared<FVoiceAllocator>();
		auto Adsr = std::make_shared<FAdsr>();
		auto Osc = std::make_shared<FOscillator>();
		auto Filt = std::make_shared<FSvf>();
		auto Lfo1 = std::make_shared<FLfo>();
		auto Lfo2 = std::make_shared<FLfo>();
		auto Matrix = std::make_shared<FModulationMatrix>();
		auto CutoffScale = std::make_shared<FScale>();
		auto GainNode = std::make_shared<FGain>();
		auto Reverb = std::make_shared<FReverb>();

		Adsr->SetParamValue(FAdsr::Param_AttackMs, 800.0f);
		Adsr->SetParamValue(FAdsr::Param_DecayMs, 800.0f);
		Adsr->SetParamValue(FAdsr::Param_Sustain, 0.85f);
		Adsr->SetParamValue(FAdsr::Param_ReleaseMs, 2000.0f);
		Osc->SetParamValue(FOscillator::Param_Shape, static_cast<float>(Saw));

		Lfo1->SetParamValue(FLfo::Param_Shape, 0.0f);  // Sine
		Lfo1->SetParamValue(FLfo::Param_RateHz, 0.3f);
		Lfo1->SetParamValue(FLfo::Param_Amount, 1.0f);
		Lfo2->SetParamValue(FLfo::Param_Shape, 1.0f);  // Triangle
		Lfo2->SetParamValue(FLfo::Param_RateHz, 0.7f);
		Lfo2->SetParamValue(FLfo::Param_Amount, 1.0f);

		// Dst0 (cutoff CV): 0.7 * Lfo1 + 0.2 * Lfo2, no offset.
		Matrix->SetParamValue(FModulationMatrix::DepthIndex(0, 0), 0.7f);
		Matrix->SetParamValue(FModulationMatrix::DepthIndex(0, 1), 0.2f);
		// Dst1 (resonance): 0.1 * Lfo1 + 0.4 * Lfo2 + 0.4 offset.
		Matrix->SetParamValue(FModulationMatrix::DepthIndex(1, 0), 0.1f);
		Matrix->SetParamValue(FModulationMatrix::DepthIndex(1, 1), 0.4f);
		Matrix->SetParamValue(FModulationMatrix::OffsetIndex(1), 0.4f);

		CutoffScale->SetParamValue(FScale::Param_InMin, -1.0f);
		CutoffScale->SetParamValue(FScale::Param_InMax, 1.0f);
		CutoffScale->SetParamValue(FScale::Param_OutMin, 250.0f);
		CutoffScale->SetParamValue(FScale::Param_OutMax, 6000.0f);

		Filt->SetParamValue(FSvf::Param_Cutoff, 2000.0f);
		Filt->SetParamValue(FSvf::Param_Resonance, 0.4f);

		Reverb->SetParamValue(FReverb::Param_RoomSize, 0.85f);
		Reverb->SetParamValue(FReverb::Param_Damping, 0.4f);
		Reverb->SetParamValue(FReverb::Param_Wet, 0.5f);
		GainNode->SetParamValue(FGain::Param_Gain, 0.16f);

		const FNodeId AllocId = M.AddNode(Alloc, 60.0f, 320.0f);
		const FNodeId AdsrId = M.AddNode(Adsr, 320.0f, 100.0f);
		const FNodeId OscId = M.AddNode(Osc, 320.0f, 320.0f);
		const FNodeId Lfo1Id = M.AddNode(Lfo1, 60.0f, 540.0f);
		const FNodeId Lfo2Id = M.AddNode(Lfo2, 60.0f, 660.0f);
		const FNodeId MatrixId = M.AddNode(Matrix, 320.0f, 600.0f);
		const FNodeId CutScaleId = M.AddNode(CutoffScale, 580.0f, 540.0f);
		const FNodeId FiltId = M.AddNode(Filt, 580.0f, 320.0f);
		const FNodeId GainId = M.AddNode(GainNode, 840.0f, 320.0f);
		const FNodeId ReverbId = M.AddNode(Reverb, 1080.0f, 320.0f);

		M.SetNodePerVoice(AdsrId, true);
		M.SetNodePerVoice(OscId, true);
		M.SetNodePerVoice(FiltId, true);

		M.AddLink(AllocId, FVoiceAllocator::Output_Gate, AdsrId, 0);
		M.AddLink(AllocId, FVoiceAllocator::Output_Frequency, OscId, FOscillator::Input_Frequency);
		M.AddLink(AdsrId, 0, OscId, FOscillator::Input_Amplitude);
		M.AddLink(OscId, 0, FiltId, FSvf::Input_Audio);

		// Two LFOs into the matrix; matrix outputs broadcast (mono ->
		// per-voice) into the per-voice SVF cutoff and resonance.
		M.AddLink(Lfo1Id, 0, MatrixId, 0);  // Src0
		M.AddLink(Lfo2Id, 0, MatrixId, 1);  // Src1
		M.AddLink(MatrixId, 0, CutScaleId, 0);  // Dst0 -> Scale
		M.AddLink(CutScaleId, 0, FiltId, FSvf::Input_Cutoff);
		M.AddLink(MatrixId, 1, FiltId, FSvf::Input_Resonance);  // Dst1 -> Res

		M.AddLink(FiltId, FSvf::Output_LowPass, GainId, 0);
		M.AddLink(GainId, 0, ReverbId, 0);

		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const FNodeId MeterId = M.AddNode(MeterNode, 1320.0f, 320.0f);
		const FNodeId OutId = M.AddNode(Out, 1560.0f, 320.0f);
		M.AddLink(ReverbId, 0, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);

		SetMeta(M, "Matrix Routing Pad",
			"Saw pad with two LFOs combined through a single ModulationMatrix\n"
			"into both filter Cutoff and Resonance — different mix depths per\n"
			"destination. Without the matrix this would be 2 LFOs * 2 destinations\n"
			"= 4 Scale + 2 Add nodes; here it's one node.");
		SaveTo(M, Root, "Pad", "Matrix Routing Pad");
	}

	// =============================================================
	// MIDI CC source showcase — a hardware knob drives filter cutoff.
	// =============================================================

	void EmitCCFilterLead(const std::filesystem::path& Root)
	{
		FGraphModel M;
		auto Alloc = std::make_shared<FVoiceAllocator>();
		auto Adsr = std::make_shared<FAdsr>();
		auto Osc = std::make_shared<FOscillator>();
		auto Filt = std::make_shared<FSvf>();
		auto Cc = std::make_shared<FMidiCC>();
		auto GainNode = std::make_shared<FGain>();
		auto Reverb = std::make_shared<FReverb>();

		Adsr->SetParamValue(FAdsr::Param_AttackMs, 6.0f);
		Adsr->SetParamValue(FAdsr::Param_DecayMs, 300.0f);
		Adsr->SetParamValue(FAdsr::Param_Sustain, 0.7f);
		Adsr->SetParamValue(FAdsr::Param_ReleaseMs, 350.0f);
		Osc->SetParamValue(FOscillator::Param_Shape, static_cast<float>(Saw));

		Filt->SetParamValue(FSvf::Param_Cutoff, 4000.0f);  // overridden by CC input
		Filt->SetParamValue(FSvf::Param_Resonance, 0.4f);

		Cc->SetParamValue(FMidiCC::Param_Cc, 74.0f);        // most synths' "brightness"
		Cc->SetParamValue(FMidiCC::Param_Channel, 0.0f);    // Omni
		Cc->SetParamValue(FMidiCC::Param_Min, 200.0f);
		Cc->SetParamValue(FMidiCC::Param_Max, 6000.0f);
		Cc->SetParamValue(FMidiCC::Param_SmoothMs, 8.0f);

		Reverb->SetParamValue(FReverb::Param_RoomSize, 0.6f);
		Reverb->SetParamValue(FReverb::Param_Damping, 0.4f);
		Reverb->SetParamValue(FReverb::Param_Wet, 0.3f);
		GainNode->SetParamValue(FGain::Param_Gain, 0.18f);

		const FNodeId AllocId = M.AddNode(Alloc, 60.0f, 280.0f);
		const FNodeId AdsrId = M.AddNode(Adsr, 320.0f, 100.0f);
		const FNodeId OscId = M.AddNode(Osc, 320.0f, 280.0f);
		const FNodeId CcId = M.AddNode(Cc, 320.0f, 460.0f);
		const FNodeId FiltId = M.AddNode(Filt, 580.0f, 280.0f);
		const FNodeId GainId = M.AddNode(GainNode, 840.0f, 280.0f);
		const FNodeId ReverbId = M.AddNode(Reverb, 1080.0f, 280.0f);

		M.SetNodePerVoice(AdsrId, true);
		M.SetNodePerVoice(OscId, true);
		M.SetNodePerVoice(FiltId, true);

		M.AddLink(AllocId, FVoiceAllocator::Output_Gate, AdsrId, 0);
		M.AddLink(AllocId, FVoiceAllocator::Output_Frequency, OscId, FOscillator::Input_Frequency);
		M.AddLink(AdsrId, 0, OscId, FOscillator::Input_Amplitude);
		M.AddLink(OscId, 0, FiltId, FSvf::Input_Audio);
		// Mono CC source -> per-voice Filter.Cutoff = broadcast.
		M.AddLink(CcId, 0, FiltId, FSvf::Input_Cutoff);
		M.AddLink(FiltId, FSvf::Output_LowPass, GainId, 0);
		M.AddLink(GainId, 0, ReverbId, 0);

		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const FNodeId MeterId = M.AddNode(MeterNode, 1320.0f, 280.0f);
		const FNodeId OutId = M.AddNode(Out, 1560.0f, 280.0f);
		M.AddLink(ReverbId, 0, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);

		SetMeta(M, "CC Filter Lead",
			"Saw lead with a MIDI CC node (CC#74) driving the per-voice SVF\n"
			"cutoff between 200 and 6000 Hz. Hit Learn on the CC node and\n"
			"twist any hardware knob to assign your own controller.");
		SaveTo(M, Root, "Lead", "CC Filter Lead");
	}

	// =============================================================
	// Wavetable showcases (WT.7) — demonstrate the wavetable
	// oscillator with a slow LFO drift and an ADSR-driven morph.
	// =============================================================

	// --- Wavetable Drift: slow LFO sweeps Position across the table ----
	void EmitWavetableDrift(const std::filesystem::path& Root)
	{
		FGraphModel M;
		auto Alloc = std::make_shared<FVoiceAllocator>();
		auto Adsr = std::make_shared<FAdsr>();
		auto Wt = std::make_shared<FWavetableOscillator>();
		auto Lfo = std::make_shared<FLfo>();
		auto LfoScale = std::make_shared<FScale>();
		auto Reverb = std::make_shared<FReverb>();
		auto GainNode = std::make_shared<FGain>();

		Adsr->SetParamValue(FAdsr::Param_AttackMs, 1500.0f);
		Adsr->SetParamValue(FAdsr::Param_DecayMs, 1500.0f);
		Adsr->SetParamValue(FAdsr::Param_Sustain, 0.85f);
		Adsr->SetParamValue(FAdsr::Param_ReleaseMs, 3000.0f);

		// Bundled wavetable — relative path resolves against the
		// <exe-dir>/wavetables/ directory at load time.
		Wt->SetParamString(FWavetableOscillator::Param_Wavetable, "AdditiveSweep.wav");
		Wt->SetParamValue(FWavetableOscillator::Param_Amplitude, 0.6f);

		Lfo->SetParamValue(FLfo::Param_Shape, 0.0f);
		Lfo->SetParamValue(FLfo::Param_RateHz, 0.12f);
		Lfo->SetParamValue(FLfo::Param_Amount, 1.0f);
		LfoScale->SetParamValue(FScale::Param_InMin, -1.0f);
		LfoScale->SetParamValue(FScale::Param_InMax, 1.0f);
		LfoScale->SetParamValue(FScale::Param_OutMin, 0.0f);
		LfoScale->SetParamValue(FScale::Param_OutMax, 1.0f);

		Reverb->SetParamValue(FReverb::Param_RoomSize, 0.85f);
		Reverb->SetParamValue(FReverb::Param_Damping, 0.4f);
		Reverb->SetParamValue(FReverb::Param_Wet, 0.55f);
		GainNode->SetParamValue(FGain::Param_Gain, 0.18f);

		const FNodeId AllocId = M.AddNode(Alloc, 60.0f, 280.0f);
		const FNodeId AdsrId = M.AddNode(Adsr, 320.0f, 100.0f);
		const FNodeId WtId = M.AddNode(Wt, 320.0f, 280.0f);
		const FNodeId LfoId = M.AddNode(Lfo, 60.0f, 460.0f);
		const FNodeId LfoScaleId = M.AddNode(LfoScale, 320.0f, 460.0f);
		const FNodeId GainId = M.AddNode(GainNode, 580.0f, 280.0f);
		const FNodeId ReverbId = M.AddNode(Reverb, 840.0f, 280.0f);

		M.SetNodePerVoice(AdsrId, true);
		M.SetNodePerVoice(WtId, true);

		M.AddLink(AllocId, FVoiceAllocator::Output_Gate, AdsrId, 0);
		M.AddLink(AllocId, FVoiceAllocator::Output_Frequency, WtId, FWavetableOscillator::Input_Frequency);
		M.AddLink(AdsrId, 0, WtId, FWavetableOscillator::Input_Amplitude);
		M.AddLink(LfoId, 0, LfoScaleId, 0);
		// Mono LfoScale.Out -> per-voice Wt.Position: broadcast.
		M.AddLink(LfoScaleId, 0, WtId, FWavetableOscillator::Input_Position);
		M.AddLink(WtId, 0, GainId, 0);
		M.AddLink(GainId, 0, ReverbId, 0);

		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const FNodeId MeterId = M.AddNode(MeterNode, 1080.0f, 280.0f);
		const FNodeId OutId = M.AddNode(Out, 1320.0f, 280.0f);
		M.AddLink(ReverbId, 0, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);

		SetMeta(M, "Wavetable Drift",
			"AdditiveSweep wavetable (16 frames sine -> saw) with a slow LFO\n"
			"morphing Position across the table over ~8 seconds. Lush pad, big reverb.");
		SaveTo(M, Root, "Pad", "Wavetable Drift");
	}

	// --- Wavetable Lead: ADSR drives both amplitude AND position --------
	void EmitWavetableLead(const std::filesystem::path& Root)
	{
		FGraphModel M;
		auto Alloc = std::make_shared<FVoiceAllocator>();
		auto Adsr = std::make_shared<FAdsr>();
		auto Wt = std::make_shared<FWavetableOscillator>();
		auto Filt = std::make_shared<FSvf>();
		auto Delay = std::make_shared<FDelay>();
		auto Reverb = std::make_shared<FReverb>();
		auto GainNode = std::make_shared<FGain>();

		Adsr->SetParamValue(FAdsr::Param_AttackMs, 4.0f);
		Adsr->SetParamValue(FAdsr::Param_DecayMs, 800.0f);
		Adsr->SetParamValue(FAdsr::Param_Sustain, 0.3f);
		Adsr->SetParamValue(FAdsr::Param_ReleaseMs, 600.0f);

		Wt->SetParamString(FWavetableOscillator::Param_Wavetable, "FMBell.wav");
		Wt->SetParamValue(FWavetableOscillator::Param_Amplitude, 0.7f);

		Filt->SetParamValue(FSvf::Param_Cutoff, 6000.0f);
		Filt->SetParamValue(FSvf::Param_Resonance, 0.3f);

		Delay->SetParamValue(FDelay::Param_TimeMs, 280.0f);
		Delay->SetParamValue(FDelay::Param_Feedback, 0.4f);
		Delay->SetParamValue(FDelay::Param_Tone, 0.6f);

		Reverb->SetParamValue(FReverb::Param_RoomSize, 0.7f);
		Reverb->SetParamValue(FReverb::Param_Damping, 0.5f);
		Reverb->SetParamValue(FReverb::Param_Wet, 0.35f);
		GainNode->SetParamValue(FGain::Param_Gain, 0.16f);

		const FNodeId AllocId = M.AddNode(Alloc, 60.0f, 240.0f);
		const FNodeId AdsrId = M.AddNode(Adsr, 320.0f, 60.0f);
		const FNodeId WtId = M.AddNode(Wt, 320.0f, 240.0f);
		const FNodeId FiltId = M.AddNode(Filt, 580.0f, 240.0f);
		const FNodeId GainId = M.AddNode(GainNode, 840.0f, 240.0f);
		const FNodeId DelayId = M.AddNode(Delay, 1080.0f, 240.0f);
		const FNodeId ReverbId = M.AddNode(Reverb, 1320.0f, 240.0f);

		M.SetNodePerVoice(AdsrId, true);
		M.SetNodePerVoice(WtId, true);
		M.SetNodePerVoice(FiltId, true);

		M.AddLink(AllocId, FVoiceAllocator::Output_Gate, AdsrId, 0);
		M.AddLink(AllocId, FVoiceAllocator::Output_Frequency, WtId, FWavetableOscillator::Input_Frequency);
		// ADSR.Env fans out to amplitude AND position — each note's
		// envelope simultaneously shapes loudness and morphs through
		// the FM bell table.
		M.AddLink(AdsrId, 0, WtId, FWavetableOscillator::Input_Amplitude);
		M.AddLink(AdsrId, 0, WtId, FWavetableOscillator::Input_Position);
		M.AddLink(WtId, 0, FiltId, FSvf::Input_Audio);
		M.AddLink(FiltId, FSvf::Output_LowPass, GainId, 0);
		M.AddLink(GainId, 0, DelayId, 0);
		M.AddLink(DelayId, 0, ReverbId, 0);

		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const FNodeId MeterId = M.AddNode(MeterNode, 1560.0f, 240.0f);
		const FNodeId OutId = M.AddNode(Out, 1800.0f, 240.0f);
		M.AddLink(ReverbId, 0, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);

		SetMeta(M, "Wavetable Lead",
			"FMBell wavetable (32-frame DX-style morph). The per-voice ADSR\n"
			"drives both oscillator amplitude AND Position — each note morphs\n"
			"from clean fundamental into a metallic bell as it decays.");
		SaveTo(M, Root, "Lead", "Wavetable Lead");
	}

	// =============================================================
	// "Crazy" batch — patches that push the engine to extremes:
	// kitchen-sink effect chains, self-oscillating filters used as
	// tone generators, wide pitch modulation, square-wave tremolo
	// stutter, etc.
	// =============================================================

	// --- Glitch Storm: every effect in series, tuned for maximum chaos --
	// Saw -> Bitcrusher -> Ring Mod (square carrier) -> AutoPan (fast) ->
	// Delay (high feedback) -> Reverb -> Limiter. The order matters: crush
	// before ring-mod so the quantization steps become tonal partials.
	void EmitGlitchStorm(const std::filesystem::path& Root)
	{
		FGraphModel M;
		FCore C = BuildPolyCore(M, 0.10f, Saw, 1.0f, 200.0f, 0.6f, 250.0f);

		auto Crush = std::make_shared<FBitcrusher>();
		auto Ring = std::make_shared<FRingMod>();
		auto Pan = std::make_shared<FAutoPan>();
		auto Delay = std::make_shared<FDelay>();
		auto Reverb = std::make_shared<FReverb>();
		auto Limit = std::make_shared<FLimiter>();
		Crush->SetParamValue(FBitcrusher::Param_SampleRateRatio, 0.12f);
		Crush->SetParamValue(FBitcrusher::Param_Bits, 4.0f);
		Crush->SetParamValue(FBitcrusher::Param_Mix, 1.0f);
		Ring->SetParamValue(FRingMod::Param_CarrierHz, 137.0f);
		Ring->SetParamValue(FRingMod::Param_Shape, 2.0f); // Square
		Ring->SetParamValue(FRingMod::Param_Mix, 0.85f);
		Pan->SetParamValue(FAutoPan::Param_Rate, 6.0f);
		Pan->SetParamValue(FAutoPan::Param_Depth, 1.0f);
		Pan->SetParamValue(FAutoPan::Param_Shape, 2.0f); // Square — hard L/R jumps
		Delay->SetParamValue(FDelay::Param_TimeMs, 187.0f);
		Delay->SetParamValue(FDelay::Param_Feedback, 0.75f);
		Delay->SetParamValue(FDelay::Param_Tone, 0.6f);
		Reverb->SetParamValue(FReverb::Param_RoomSize, 0.85f);
		Reverb->SetParamValue(FReverb::Param_Damping, 0.4f);
		Reverb->SetParamValue(FReverb::Param_Wet, 0.4f);
		Limit->SetParamValue(FLimiter::Param_CeilingDb, -1.5f);
		Limit->SetParamValue(FLimiter::Param_ReleaseMs, 30.0f);

		const FNodeId CrushId = M.AddNode(Crush, 860.0f, 180.0f);
		const FNodeId RingId = M.AddNode(Ring, 1100.0f, 180.0f);
		const FNodeId PanId = M.AddNode(Pan, 1340.0f, 180.0f);
		const FNodeId DelayId = M.AddNode(Delay, 1580.0f, 180.0f);
		const FNodeId ReverbId = M.AddNode(Reverb, 1820.0f, 180.0f);
		const FNodeId LimitId = M.AddNode(Limit, 2060.0f, 180.0f);
		M.AddLink(C.LastId, C.LastOutPort, CrushId, 0);
		M.AddLink(CrushId, 0, RingId, 0);
		M.AddLink(RingId, 0, PanId, 0);
		M.AddLink(PanId, 0, DelayId, 0);
		M.AddLink(DelayId, 0, ReverbId, 0);
		M.AddLink(ReverbId, 0, LimitId, 0);
		C.LastId = LimitId;

		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const FNodeId MeterId = M.AddNode(MeterNode, 2300.0f, 180.0f);
		const FNodeId OutId = M.AddNode(Out, 2540.0f, 180.0f);
		M.AddLink(LimitId, 0, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);

		SetMeta(M, "Glitch Storm",
			"Saw -> 4-bit Bitcrusher -> Square Ring Mod (137 Hz) -> Square AutoPan (6 Hz) -> Delay (75% FB) -> Reverb -> Limiter. Six effects in series; the limiter is doing actual work.");
		SaveTo(M, Root, "FX", "Glitch Storm");
	}

	// --- Drone Choir: extreme attack/release, 3-voice chorus + phaser ---
	// Glacial envelope, dense modulation. Each note takes 4 seconds to
	// fully bloom and 8 seconds to die. Stack two or three notes for a
	// huge evolving texture.
	void EmitDroneChoir(const std::filesystem::path& Root)
	{
		FGraphModel M;
		FCore C = BuildPolyCore(M, 0.10f, Saw, 4000.0f, 3000.0f, 0.95f, 8000.0f);

		auto Chorus = std::make_shared<FChorus>();
		auto Phaser = std::make_shared<FPhaser>();
		auto Wide = std::make_shared<FStereoWidener>();
		auto Reverb = std::make_shared<FReverb>();
		Chorus->SetParamValue(FChorus::Param_Rate, 0.18f);
		Chorus->SetParamValue(FChorus::Param_Depth, 0.85f);
		Chorus->SetParamValue(FChorus::Param_Mix, 0.7f);
		Chorus->SetParamValue(FChorus::Param_Voices, 2.0f); // 3 voices
		Phaser->SetParamValue(FPhaser::Param_Rate, 0.12f);
		Phaser->SetParamValue(FPhaser::Param_Depth, 0.85f);
		Phaser->SetParamValue(FPhaser::Param_Feedback, 0.55f);
		Phaser->SetParamValue(FPhaser::Param_Mix, 0.55f);
		Phaser->SetParamValue(FPhaser::Param_Stages, 2.0f); // 8 stages
		Wide->SetParamValue(FStereoWidener::Param_Width, 1.7f);
		Reverb->SetParamValue(FReverb::Param_RoomSize, 0.95f);
		Reverb->SetParamValue(FReverb::Param_Damping, 0.25f);
		Reverb->SetParamValue(FReverb::Param_Wet, 0.6f);

		const FNodeId ChorusId = M.AddNode(Chorus, 860.0f, 180.0f);
		const FNodeId PhaserId = M.AddNode(Phaser, 1100.0f, 180.0f);
		const FNodeId WideId = M.AddNode(Wide, 1340.0f, 180.0f);
		const FNodeId ReverbId = M.AddNode(Reverb, 1580.0f, 180.0f);
		M.AddLink(C.LastId, C.LastOutPort, ChorusId, 0);
		M.AddLink(ChorusId, 0, PhaserId, 0);
		M.AddLink(PhaserId, 0, WideId, 0);
		M.AddLink(WideId, 0, ReverbId, 0);
		C.LastId = ReverbId;

		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const FNodeId MeterId = M.AddNode(MeterNode, 1820.0f, 180.0f);
		const FNodeId OutId = M.AddNode(Out, 2060.0f, 180.0f);
		M.AddLink(ReverbId, 0, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);

		SetMeta(M, "Drone Choir",
			"4 s attack / 8 s release Saw pad -> 3-voice Chorus -> 8-stage Phaser -> Stereo Widener -> 95% Reverb. Hold a chord and let it bloom. Massive evolving texture.");
		SaveTo(M, Root, "Pad", "Drone Choir");
	}

	// --- Pitch Storm: LFO added to per-voice oscillator frequency -------
	// Mono LFO (slow) is added to the allocator's frequency before it
	// reaches the oscillator — every voice's pitch wobbles by ±400 Hz.
	// On a held chord this turns into a swarm of cross-detuning sirens.
	void EmitPitchStorm(const std::filesystem::path& Root)
	{
		FGraphModel M;
		auto Alloc = std::make_shared<FVoiceAllocator>();
		auto Adsr = std::make_shared<FAdsr>();
		auto Lfo = std::make_shared<FLfo>();
		auto Add = std::make_shared<FAdd>();
		auto Osc = std::make_shared<FOscillator>();
		auto Crush = std::make_shared<FBitcrusher>();
		auto Reverb = std::make_shared<FReverb>();
		auto Limit = std::make_shared<FLimiter>();
		auto GainNode = std::make_shared<FGain>();

		Adsr->SetParamValue(FAdsr::Param_AttackMs, 50.0f);
		Adsr->SetParamValue(FAdsr::Param_DecayMs, 400.0f);
		Adsr->SetParamValue(FAdsr::Param_Sustain, 0.7f);
		Adsr->SetParamValue(FAdsr::Param_ReleaseMs, 800.0f);
		Lfo->SetParamValue(FLfo::Param_Shape, 0.0f);   // Sine
		Lfo->SetParamValue(FLfo::Param_RateHz, 0.45f);
		Lfo->SetParamValue(FLfo::Param_Amount, 400.0f); // ±400 Hz (huge)
		Osc->SetParamValue(FOscillator::Param_Shape, static_cast<float>(Saw));
		Crush->SetParamValue(FBitcrusher::Param_SampleRateRatio, 0.35f);
		Crush->SetParamValue(FBitcrusher::Param_Bits, 8.0f);
		Crush->SetParamValue(FBitcrusher::Param_Mix, 0.55f);
		Reverb->SetParamValue(FReverb::Param_RoomSize, 0.85f);
		Reverb->SetParamValue(FReverb::Param_Damping, 0.35f);
		Reverb->SetParamValue(FReverb::Param_Wet, 0.5f);
		Limit->SetParamValue(FLimiter::Param_CeilingDb, -1.0f);
		Limit->SetParamValue(FLimiter::Param_ReleaseMs, 60.0f);
		GainNode->SetParamValue(FGain::Param_Gain, 0.14f);

		const FNodeId AllocId = M.AddNode(Alloc, 60.0f, 280.0f);
		const FNodeId AdsrId = M.AddNode(Adsr, 320.0f, 100.0f);
		const FNodeId LfoId = M.AddNode(Lfo, 60.0f, 460.0f);
		const FNodeId AddId = M.AddNode(Add, 320.0f, 280.0f);
		const FNodeId OscId = M.AddNode(Osc, 580.0f, 280.0f);
		const FNodeId GainId = M.AddNode(GainNode, 840.0f, 280.0f);
		const FNodeId CrushId = M.AddNode(Crush, 1080.0f, 280.0f);
		const FNodeId ReverbId = M.AddNode(Reverb, 1320.0f, 280.0f);
		const FNodeId LimitId = M.AddNode(Limit, 1560.0f, 280.0f);

		M.SetNodePerVoice(AdsrId, true);
		M.SetNodePerVoice(AddId, true);
		M.SetNodePerVoice(OscId, true);

		// Allocator.Freq (per-voice) + LFO.Out (mono, broadcast) -> Osc.Freq.
		M.AddLink(AllocId, FVoiceAllocator::Output_Gate, AdsrId, 0);
		M.AddLink(AllocId, FVoiceAllocator::Output_Frequency, AddId, 0);
		M.AddLink(LfoId, 0, AddId, 1);
		M.AddLink(AddId, 0, OscId, FOscillator::Input_Frequency);
		M.AddLink(AdsrId, 0, OscId, FOscillator::Input_Amplitude);
		M.AddLink(OscId, 0, GainId, 0);
		M.AddLink(GainId, 0, CrushId, 0);
		M.AddLink(CrushId, 0, ReverbId, 0);
		M.AddLink(ReverbId, 0, LimitId, 0);

		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const FNodeId MeterId = M.AddNode(MeterNode, 1800.0f, 280.0f);
		const FNodeId OutId = M.AddNode(Out, 2040.0f, 280.0f);
		M.AddLink(LimitId, 0, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);

		SetMeta(M, "Pitch Storm",
			"Mono LFO (Amount=400 Hz) added to the per-voice allocator frequency through a per-voice Add node. Every held voice cross-detunes against its neighbours. Bitcrusher + Reverb sweetens the chaos.");
		SaveTo(M, Root, "FX", "Pitch Storm");
	}

	// --- Filter Bell: self-oscillating SVF as a tone generator ----------
	// The oscillator only fires a tiny noise burst on each note. The SVF
	// (resonance ~ 0.99) self-oscillates at its cutoff frequency, which is
	// driven directly from the allocator's frequency output. So the FILTER
	// is making the pitched tone, not the oscillator.
	void EmitFilterBell(const std::filesystem::path& Root)
	{
		FGraphModel M;
		auto Alloc = std::make_shared<FVoiceAllocator>();
		auto Adsr = std::make_shared<FAdsr>();
		auto NoiseOsc = std::make_shared<FOscillator>();
		auto Filt = std::make_shared<FSvf>();
		auto Reverb = std::make_shared<FReverb>();
		auto GainNode = std::make_shared<FGain>();

		Adsr->SetParamValue(FAdsr::Param_AttackMs, 1.0f);
		Adsr->SetParamValue(FAdsr::Param_DecayMs, 8.0f);    // very short noise burst
		Adsr->SetParamValue(FAdsr::Param_Sustain, 0.0f);
		Adsr->SetParamValue(FAdsr::Param_ReleaseMs, 8.0f);
		NoiseOsc->SetParamValue(FOscillator::Param_Shape, static_cast<float>(Noise));
		Filt->SetParamValue(FSvf::Param_Resonance, 0.99f);  // self-oscillation
		Reverb->SetParamValue(FReverb::Param_RoomSize, 0.92f);
		Reverb->SetParamValue(FReverb::Param_Damping, 0.2f);
		Reverb->SetParamValue(FReverb::Param_Wet, 0.6f);
		GainNode->SetParamValue(FGain::Param_Gain, 0.14f);

		const FNodeId AllocId = M.AddNode(Alloc, 60.0f, 240.0f);
		const FNodeId AdsrId = M.AddNode(Adsr, 320.0f, 60.0f);
		const FNodeId NoiseId = M.AddNode(NoiseOsc, 320.0f, 240.0f);
		const FNodeId FiltId = M.AddNode(Filt, 580.0f, 240.0f);
		const FNodeId GainId = M.AddNode(GainNode, 840.0f, 240.0f);
		const FNodeId ReverbId = M.AddNode(Reverb, 1080.0f, 240.0f);

		M.SetNodePerVoice(AdsrId, true);
		M.SetNodePerVoice(NoiseId, true);
		M.SetNodePerVoice(FiltId, true);

		M.AddLink(AllocId, FVoiceAllocator::Output_Gate, AdsrId, 0);
		M.AddLink(AdsrId, 0, NoiseId, FOscillator::Input_Amplitude);
		// Allocator.Freq -> Filter.Cutoff: the filter rings at note pitch.
		M.AddLink(AllocId, FVoiceAllocator::Output_Frequency, FiltId, FSvf::Input_Cutoff);
		M.AddLink(NoiseId, 0, FiltId, FSvf::Input_Audio);
		M.AddLink(FiltId, FSvf::Output_BandPass, GainId, 0);
		M.AddLink(GainId, 0, ReverbId, 0);

		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const FNodeId MeterId = M.AddNode(MeterNode, 1320.0f, 240.0f);
		const FNodeId OutId = M.AddNode(Out, 1560.0f, 240.0f);
		M.AddLink(ReverbId, 0, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);

		SetMeta(M, "Filter Bell",
			"The oscillator only emits an 8 ms noise click on each note. A near-self-oscillating SVF (resonance 0.99) tuned to the note frequency rings out as the actual tone. The FILTER is the sound source.");
		SaveTo(M, Root, "FX", "Filter Bell");
	}

	// --- Stutter Lead: square-wave Tremolo at 14 Hz on a filtered saw ---
	// Square Tremolo with Depth=1 and a fast rate creates a hard on/off
	// gate at the tremolo frequency, turning a smooth saw into a stuttered
	// machine-gun lead. Pair with delay so each "click" trails into the
	// next stutter pulse.
	void EmitStutterLead(const std::filesystem::path& Root)
	{
		FGraphModel M;
		FCore C = BuildPolyCore(M, 0.16f, Saw, 4.0f, 200.0f, 0.7f, 250.0f);

		auto Filt = std::make_shared<FSvf>();
		auto Trem = std::make_shared<FTremolo>();
		auto Delay = std::make_shared<FDelay>();
		auto Reverb = std::make_shared<FReverb>();
		Filt->SetParamValue(FSvf::Param_Cutoff, 2200.0f);
		Filt->SetParamValue(FSvf::Param_Resonance, 0.55f);
		Trem->SetParamValue(FTremolo::Param_Rate, 14.0f);
		Trem->SetParamValue(FTremolo::Param_Depth, 1.0f);
		Trem->SetParamValue(FTremolo::Param_Shape, 2.0f); // Square
		Trem->SetParamValue(FTremolo::Param_Stereo, 1.0f); // Quad — alternating L/R stutter
		Delay->SetParamValue(FDelay::Param_TimeMs, 142.0f);
		Delay->SetParamValue(FDelay::Param_Feedback, 0.5f);
		Delay->SetParamValue(FDelay::Param_Tone, 0.55f);
		Reverb->SetParamValue(FReverb::Param_RoomSize, 0.7f);
		Reverb->SetParamValue(FReverb::Param_Damping, 0.4f);
		Reverb->SetParamValue(FReverb::Param_Wet, 0.3f);

		const FNodeId FiltId = M.AddNode(Filt, 860.0f, 180.0f);
		const FNodeId TremId = M.AddNode(Trem, 1100.0f, 180.0f);
		const FNodeId DelayId = M.AddNode(Delay, 1340.0f, 180.0f);
		const FNodeId ReverbId = M.AddNode(Reverb, 1580.0f, 180.0f);
		M.AddLink(C.LastId, C.LastOutPort, FiltId, FSvf::Input_Audio);
		M.AddLink(FiltId, FSvf::Output_LowPass, TremId, 0);
		M.AddLink(TremId, 0, DelayId, 0);
		M.AddLink(DelayId, 0, ReverbId, 0);
		C.LastId = ReverbId;

		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const FNodeId MeterId = M.AddNode(MeterNode, 1820.0f, 180.0f);
		const FNodeId OutId = M.AddNode(Out, 2060.0f, 180.0f);
		M.AddLink(ReverbId, 0, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);

		SetMeta(M, "Stutter Lead",
			"Saw -> resonant LP -> Square Tremolo (14 Hz, depth 1, L/R offset) -> Delay -> Reverb. The tremolo hard-gates the audio at 14 Hz; alternating L/R produces a stereo machine-gun.");
		SaveTo(M, Root, "Lead", "Stutter Lead");
	}

	// --- Auto-Pan Pluck: triangle → auto-pan → haas widener → reverb -----
	void EmitAutoPanPluck(const std::filesystem::path& Root)
	{
		FGraphModel M;
		FCore C = BuildPolyCore(M, 0.16f, Triangle, 1.0f, 280.0f, 0.0f, 280.0f);

		auto Pan = std::make_shared<FAutoPan>();
		auto Haas = std::make_shared<FHaasWidener>();
		auto Reverb = std::make_shared<FReverb>();
		Pan->SetParamValue(FAutoPan::Param_Rate, 1.0f);
		Pan->SetParamValue(FAutoPan::Param_Depth, 0.85f);
		Pan->SetParamValue(FAutoPan::Param_Shape, 0.0f); // Sine
		Haas->SetParamValue(FHaasWidener::Param_DelayMs, 12.0f);
		Haas->SetParamValue(FHaasWidener::Param_Side, 0.0f); // delay R
		Haas->SetParamValue(FHaasWidener::Param_Mix, 0.7f);
		Reverb->SetParamValue(FReverb::Param_RoomSize, 0.6f);
		Reverb->SetParamValue(FReverb::Param_Damping, 0.4f);
		Reverb->SetParamValue(FReverb::Param_Wet, 0.3f);

		const FNodeId PanId = M.AddNode(Pan, 860.0f, 180.0f);
		const FNodeId HaasId = M.AddNode(Haas, 1100.0f, 180.0f);
		const FNodeId ReverbId = M.AddNode(Reverb, 1340.0f, 180.0f);
		M.AddLink(C.LastId, C.LastOutPort, PanId, 0);
		M.AddLink(PanId, 0, HaasId, 0);
		M.AddLink(HaasId, 0, ReverbId, 0);
		C.LastId = ReverbId;
		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const FNodeId MeterId = M.AddNode(MeterNode, 1580.0f, 180.0f);
		const FNodeId OutId = M.AddNode(Out, 1820.0f, 180.0f);
		M.AddLink(ReverbId, 0, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);

		SetMeta(M, "Auto-Pan Pluck",
			"Triangle pluck -> Auto-Pan (1 Hz) -> Haas Widener -> Reverb. Each pluck swings across the stereo field with a precedence-effect smear.");
		SaveTo(M, Root, "FX", "Auto-Pan Pluck");
	}

	// --- Vocoder Talk: live mic modulates a saw-drone carrier -----------
	// The "talking synth". Mic Input (modulator) imposes its spectral
	// envelope onto a saw Oscillator drone (carrier) via the Vocoder. Mono
	// throughout; the mic device defaults to "Off" in the saved patch, so the
	// user picks a capture device in the Mic Input panel to bring it to life.
	void EmitVocoderTalk(const std::filesystem::path& Root)
	{
		FGraphModel M;
		auto Mic = std::make_shared<FMicInput>();
		auto Osc = std::make_shared<FOscillator>();
		auto Voc = std::make_shared<FVocoder>();
		auto GainNode = std::make_shared<FGain>();

		Osc->SetParamValue(FOscillator::Param_Shape, static_cast<float>(Saw));
		Osc->SetParamValue(FOscillator::Param_Frequency, 110.0f);  // A2 drone
		Osc->SetParamValue(FOscillator::Param_Amplitude, 0.8f);
		Voc->SetParamValue(FVocoder::Param_Bands, 1.0f);    // 16 bands
		Voc->SetParamValue(FVocoder::Param_Attack, 5.0f);
		Voc->SetParamValue(FVocoder::Param_Release, 40.0f);
		Voc->SetParamValue(FVocoder::Param_Formant, 1.0f);
		Voc->SetParamValue(FVocoder::Param_Mix, 1.0f);
		Voc->SetParamValue(FVocoder::Param_OutputDb, 12.0f);  // vocoding loses level
		GainNode->SetParamValue(FGain::Param_Gain, 0.5f);

		const FNodeId OscId = M.AddNode(Osc, 60.0f, 180.0f);
		const FNodeId MicId = M.AddNode(Mic, 60.0f, 360.0f);
		const FNodeId VocId = M.AddNode(Voc, 360.0f, 240.0f);
		const FNodeId GainId = M.AddNode(GainNode, 640.0f, 240.0f);

		M.AddLink(OscId, 0, VocId, FVocoder::Input_Carrier);
		M.AddLink(MicId, 0, VocId, FVocoder::Input_Modulator);
		M.AddLink(VocId, 0, GainId, 0);

		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();
		const FNodeId MeterId = M.AddNode(MeterNode, 880.0f, 240.0f);
		const FNodeId OutId = M.AddNode(Out, 1120.0f, 240.0f);
		M.AddLink(GainId, 0, MeterId, 0);
		M.AddLink(MeterId, 0, OutId, 0);

		SetMeta(M, "Vocoder Talk",
			"Talking synth. Mic Input -> Vocoder Modulator; a 110 Hz saw Oscillator drone -> Carrier. "
			"Open the Mic Input node, pick a capture device, and speak or sing — the saw 'says' your words. "
			"Repitch the oscillator to change the robot's note. USE HEADPHONES: speaker monitoring feeds back into the mic.");
		SaveTo(M, Root, "FX", "Vocoder Talk");
	}
}

TEST_CASE("Emit bundled presets to ./presets/", "[.][preset-emit]")
{
	const std::filesystem::path Root = std::filesystem::current_path() / "presets";
	std::filesystem::create_directories(Root);

	// Init: identical to main.cpp::SeedDefaultPatch defaults.
	EmitPreset(Root, "Init", "Init Patch",
		"Polyphonic sine, 8 voices, master gain 0.15. The seeded default patch.",
		Sine, 5.0f, 200.0f, 0.7f, 400.0f, 0.15f, 0.0f);

	// Bass: short release, square sub.
	EmitPreset(Root, "Bass", "Square Sub",
		"Square wave with snappy envelope. Pull the master gain up if you only play one voice at a time.",
		Square, 1.0f, 80.0f, 0.6f, 120.0f, 0.18f, 0.0f);

	EmitPreset(Root, "Bass", "Acid Saw",
		"Saw with a fast attack and quick release. Pair with an SVF for resonant acid lines.",
		Saw, 1.0f, 100.0f, 0.5f, 80.0f, 0.15f, 0.0f);

	// Lead: faster attack, brighter shapes.
	EmitPreset(Root, "Lead", "Saw Lead",
		"Saw lead with a touch of glide. Plays well above middle C.",
		Saw, 8.0f, 200.0f, 0.7f, 350.0f, 0.18f, 30.0f);

	EmitPreset(Root, "Lead", "Square Lead",
		"Hollow square lead. 60 ms glide gives it a vocal quality on legato runs.",
		Square, 12.0f, 220.0f, 0.65f, 300.0f, 0.18f, 60.0f);

	// Pad: long attack/release, sustained.
	EmitPreset(Root, "Pad", "Soft Pad",
		"Triangle pad with slow attack and long release. Holds a chord nicely.",
		Triangle, 600.0f, 1000.0f, 0.85f, 2200.0f, 0.18f, 0.0f);

	EmitPreset(Root, "Pad", "String",
		"Saw-based string pad. Slow attack, long release, fairly bright.",
		Saw, 800.0f, 1200.0f, 0.8f, 2500.0f, 0.16f, 0.0f);

	// FX: percussive plucks.
	EmitPreset(Root, "FX", "Pluck",
		"Triangle pluck — fast attack, decay-only envelope (sustain=0).",
		Triangle, 1.0f, 250.0f, 0.0f, 250.0f, 0.18f, 0.0f);

	// Advanced presets — each demonstrates one or more of the Phase-5c /
	// Stage-E effect nodes. Topologies diverge from the seeded skeleton, so
	// these are emitted via dedicated functions instead of EmitPreset.
	EmitLushPad(Root);
	EmitWidePad(Root);
	EmitPhasedLead(Root);
	EmitGrittyBass(Root);
	EmitBellTone(Root);
	EmitCrushedStab(Root);
	EmitDubStab(Root);
	EmitTremoloLead(Root);
	EmitAutoPanPluck(Root);

	// Capability-showcase batch — ADSR-modulated filters, mono->per-voice
	// LFO broadcast, self-playing sequencer, mastering chain, S&H stepped
	// modulation, detuned super-saw via per-voice Math + Mixer.
	EmitFilterSweepLead(Root);
	EmitWobbleBass(Root);
	EmitAcidSequence(Root);
	EmitMasteredPad(Root);
	EmitSteppedLfoBass(Root);
	EmitSuperSaw(Root);

	// "Crazy" batch — extreme effect chains, self-oscillating filters,
	// huge LFO pitch swings, square-wave stutter, glacial drone.
	EmitGlitchStorm(Root);
	EmitDroneChoir(Root);
	EmitPitchStorm(Root);
	EmitFilterBell(Root);
	EmitStutterLead(Root);

	// Wavetable showcases — load bundled WAVs by relative path.
	EmitWavetableDrift(Root);
	EmitWavetableLead(Root);

	// MIDI CC source showcase.
	EmitCCFilterLead(Root);

	// Modulation Matrix showcase.
	EmitMatrixRoutingPad(Root);

	// Vocoder + live Mic Input showcase ("talking synth").
	EmitVocoderTalk(Root);
}
