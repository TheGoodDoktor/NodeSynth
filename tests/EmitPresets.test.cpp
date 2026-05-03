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

#include "dsp/Adsr.h"
#include "dsp/AutoPan.h"
#include "dsp/Bitcrusher.h"
#include "dsp/Chorus.h"
#include "dsp/Compressor.h"
#include "dsp/Delay.h"
#include "dsp/Equalizer.h"
#include "dsp/Exciter.h"
#include "dsp/Flanger.h"
#include "dsp/Gain.h"
#include "dsp/HaasWidener.h"
#include "dsp/Limiter.h"
#include "dsp/Meter.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"
#include "dsp/Phaser.h"
#include "dsp/Reverb.h"
#include "dsp/RingMod.h"
#include "dsp/StereoWidener.h"
#include "dsp/Tremolo.h"
#include "dsp/VoiceAllocator.h"
#include "dsp/Waveshaper.h"
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

	void SaveTo(const FGraphModel& M, const std::filesystem::path& Root,
		const std::string& Category, const std::string& Name)
	{
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
}
