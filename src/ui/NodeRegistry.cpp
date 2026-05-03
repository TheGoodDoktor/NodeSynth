#include "ui/NodeRegistry.h"

#include <cstring>

#include "dsp/Add.h"
#include "dsp/Adsr.h"
#include "dsp/AutoPan.h"
#include "dsp/Bitcrusher.h"
#include "dsp/Chorus.h"
#include "dsp/Clock.h"
#include "dsp/Compressor.h"
#include "dsp/Constant.h"
#include "dsp/DcBlocker.h"
#include "dsp/Delay.h"
#include "dsp/Equalizer.h"
#include "dsp/Exciter.h"
#include "dsp/Flanger.h"
#include "dsp/Gain.h"
#include "dsp/Gate.h"
#include "dsp/GateButton.h"
#include "dsp/HaasWidener.h"
#include "dsp/Limiter.h"
#include "dsp/Lfo.h"
#include "dsp/Meter.h"
#include "dsp/Mixer.h"
#include "dsp/Multiply.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"
#include "dsp/Phaser.h"
#include "dsp/Reverb.h"
#include "dsp/RingMod.h"
#include "dsp/SampleHold.h"
#include "dsp/Scale.h"
#include "dsp/Scope.h"
#include "dsp/Sequencer.h"
#include "dsp/StereoWidener.h"
#include "dsp/SidPlayer.h"
#include "dsp/Svf.h"
#include "dsp/Tremolo.h"
#include "dsp/Vca.h"
#include "dsp/VoiceAllocator.h"
#include "dsp/Waveshaper.h"

namespace NodeSynth
{
	const std::vector<FNodeRegistration>& GetNodeRegistry()
	{
		static const std::vector<FNodeRegistration> Registry =
		{
			{
				"Oscillator", "Oscillator",
				"Audio source. Sine, saw, square, triangle, or noise.\n"
				"Inputs: Freq, Amp (Control). Output: audio.\n"
				"Saw / square / triangle use PolyBLEP to suppress aliasing.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FOscillator>(); },
			},
			{
				"Mixer", "Mixer",
				"4-channel audio mixer with per-channel gain. Sums In1..In4 (each\n"
				"scaled by its smoothed gain) into a single Out. Mark per-voice to\n"
				"layer multiple oscillators per voice (saw + sub + noise etc.).",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FMixer>(); },
			},
			{
				"Gain", "Gain",
				"Constant audio scaler. Multiplies the input signal by a smoothed gain value.\n"
				"Use it for level trim or simple mixing.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FGain>(); },
			},
			{
				"VCA", "VCA",
				"Voltage-controlled amplifier. Multiplies an audio signal by a control input.\n"
				"Wire an envelope (e.g. ADSR) into the control port to shape note amplitude.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FVca>(); },
			},
			{
				"SVF", "SVF",
				"State-variable filter (TPT / ZDF form).\n"
				"Low / high / band-pass outputs with resonance up to self-oscillation.\n"
				"Cutoff is clamped to a safe range every sample.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FSvf>(); },
			},
			{
				"ADSR", "ADSR",
				"Attack / Decay / Sustain / Release envelope generator.\n"
				"Input: Gate (Control). Output: Env (Control), 0..1.\n"
				"Re-trigger preserves the current level so retriggers don't click.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FAdsr>(); },
			},
			{
				"Gate", "Gate",
				"Manual gate toggle. Held = 1, released = 0.\n"
				"Useful for testing envelopes / VCAs without a MIDI source.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FGateButton>(); },
			},
			{
				"Output", "Output",
				"Audio sink. Routes the incoming audio buffer to the device output.\n"
				"The graph compiler treats this as the root and only walks nodes\n"
				"reachable from it.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FOutput>(); },
			},
			{
				"Add", "Add",
				"Adds two Control signals: Out = A + B.\n"
				"Disconnected inputs read as 0, so the node passes through whatever\n"
				"is connected.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FAdd>(); },
			},
			{
				"Multiply", "Multiply",
				"Multiplies two Control signals: Out = A * B.\n"
				"Disconnected inputs read as 1 (multiplication identity), so the node\n"
				"passes through whatever is connected.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FMultiply>(); },
			},
			{
				"Scale", "Scale",
				"Linear range remapper. Maps [InMin, InMax] onto [OutMin, OutMax].\n"
				"Defaults remap a bipolar [-1, 1] LFO to unipolar [0, 1].\n"
				"Values outside InMin..InMax are extrapolated, not clamped.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FScale>(); },
			},
			{
				"Constant", "Constant",
				"Constant Control source. Outputs the Value param continuously.\n"
				"Useful for fixed offsets, bias points, or as a knob you can route\n"
				"into any Control input.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FConstant>(); },
			},
			{
				"SampleHold", "Sample & Hold",
				"Latches the In value on each rising edge of Trigger and holds it\n"
				"on the output until the next rising edge.\n"
				"Classic with a noise source for random stepped modulation.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FSampleHold>(); },
			},
			{
				"LFO", "LFO",
				"Low-frequency oscillator. Bipolar output [-Amount, +Amount].\n"
				"Shapes: Sine, Triangle, Saw, Square. Rate 0.01..50 Hz.\n"
				"Sync input resets phase on rising edge — leave disconnected for\n"
				"free-running. Use a Scale node to remap to a unipolar range.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FLfo>(); },
			},
			{
				"Delay", "Delay",
				"Feedback delay line. Audio input into a 2-second buffer; output is the\n"
				"delayed signal. Feedback feeds the (tone-damped) delayed signal back\n"
				"into the buffer for repeating echoes. Connect a Control input to the\n"
				"Time port (e.g. an LFO) for chorus / flanger effects — values modulate\n"
				"per-sample without smoothing.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FDelay>(); },
			},
			{
				"Reverb", "Reverb",
				"Freeverb-style reverb (8 lowpass-feedback combs + 4 allpass diffusers).\n"
				"Mono in / mono out. Wet/dry mix is internal so the node drops in as\n"
				"a passthrough effect without an external blender.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FReverb>(); },
			},
			{
				"Chorus", "Chorus",
				"Stereo modulated-delay chorus. Two delay lines (one per channel)\n"
				"with their tap positions modulated by an LFO; L and R LFOs are\n"
				"90° out of phase for stereo width. Optional 1/2/3 voice stack.\n"
				"No feedback path — that's flanger territory.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FChorus>(); },
			},
			{
				"Flanger", "Flanger",
				"Stereo flanger: short modulated delay (≤10 ms) with a feedback\n"
				"path that creates the comb-filter sweep. Negative feedback inverts\n"
				"the comb's harmonic emphasis. L and R LFOs are 90° out of phase.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FFlanger>(); },
			},
			{
				"Phaser", "Phaser",
				"Stereo all-pass-cascade phaser. 4/6/8 stages, signed feedback,\n"
				"exponential LFO sweep. Different from chorus / flanger:\n"
				"modulates filter phase, not delay time.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FPhaser>(); },
			},
			{
				"Compressor", "Compressor",
				"Stereo peak compressor with linked detection: max(|L|, |R|)\n"
				"feeds the envelope follower; gain reduction applies identically\n"
				"to both channels so the stereo image stays intact.\n"
				"Hard-knee, no lookahead.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FCompressor>(); },
			},
			{
				"Limiter", "Limiter",
				"Stereo brickwall-style limiter — compressor with infinite\n"
				"ratio and hard knee. Ceiling caps output amplitude. No\n"
				"lookahead in v1; very fast transients can briefly slip past.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FLimiter>(); },
			},
			{
				"NoiseGate", "Noise Gate",
				"Stereo downward expander / noise gate. Below threshold the\n"
				"signal is reduced by (threshold - input) × (1 - 1/Ratio).\n"
				"Hold time prevents chatter on signals around the threshold.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FGate>(); },
			},
			{
				"Equalizer", "Equalizer",
				"Stereo 3-band EQ: low-shelf + peak + high-shelf in series.\n"
				"All three gains at 0 dB → bit-identical passthrough. Standard\n"
				"musical-EQ topology covering tilt + midrange surgery.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FEqualizer>(); },
			},
			{
				"DcBlocker", "DC Blocker",
				"Single-pole highpass at 20 Hz. Removes DC offset that\n"
				"waveshapers and other nonlinearities can introduce. No\n"
				"params — DC is a problem with one solution.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FDcBlocker>(); },
			},
			{
				"Tremolo", "Tremolo",
				"LFO modulating amplitude. Mono mode ties L/R together;\n"
				"Quad mode runs them 180° apart for ping-pong amplitude.\n"
				"Sine / Triangle / Square / Saw shapes.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FTremolo>(); },
			},
			{
				"AutoPan", "Auto-Pan",
				"LFO modulating L/R balance with constant-power pan curve.\n"
				"Mono input becomes panned-stereo output, sweeping between\n"
				"full-L and full-R. L² + R² ≈ Input² always.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FAutoPan>(); },
			},
			{
				"Bitcrusher", "Bitcrusher",
				"Sample-rate reduction (sample-and-hold) + bit-depth quantize.\n"
				"Aliasing IS the effect. Rate=1, Bits=16 → near-passthrough.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FBitcrusher>(); },
			},
			{
				"RingMod", "Ring Modulator",
				"Input × internal sine/triangle/square carrier. Produces\n"
				"sum + difference frequencies for that classic metallic /\n"
				"bell timbre.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FRingMod>(); },
			},
			{
				"StereoWidener", "Stereo Widener",
				"Mid-side processing — single Width param scales the side\n"
				"content. 0 = mono, 1 = passthrough, 2 = exaggerated.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FStereoWidener>(); },
			},
			{
				"HaasWidener", "Haas Widener",
				"Fixed short delay (5–25 ms) on one channel for apparent\n"
				"stereo width via the precedence effect. Comb-filters when\n"
				"summed to mono — pad / lead use only.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FHaasWidener>(); },
			},
			{
				"Exciter", "Exciter",
				"Highpass → tanh saturation → mix back with dry. Adds\n"
				"high-frequency harmonics for 'presence' / 'air'.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FExciter>(); },
			},
			{
				"Waveshaper", "Waveshaper",
				"Memoryless distortion. Drive (dB) pushes the signal into the\n"
				"nonlinear region; the chosen Shape (tanh / hard / soft / fold)\n"
				"determines the curve; Output (dB) compensates loudness.\n"
				"Aliases at high drive — oversampling is a Phase 5 deliverable.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FWaveshaper>(); },
			},
			{
				"Clock", "Clock",
				"Square-wave gate at a configurable BPM. Drives a Sequencer's Clock\n"
				"input out of the box. One pulse per beat (50%% duty cycle).",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FClock>(); },
			},
			{
				"Sequencer", "Sequencer",
				"16-step sequencer driven by an external Clock. Per-step pitch\n"
				"(semitones from RootNote), velocity, gate length, and enable.\n"
				"Outputs Gate / Frequency / Velocity Control signals — feed Frequency\n"
				"into an Oscillator and Gate into an ADSR for a classic sequenced lead.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FSequencer>(); },
			},
			{
				"Scope", "Scope",
				"Audio passthrough that captures the most recent samples for\n"
				"visualisation in the property panel. Drop one anywhere in the\n"
				"chain to inspect the signal shape.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FScope>(); },
			},
			{
				"Meter", "Meter",
				"Audio passthrough that exposes peak and RMS levels (in dBFS)\n"
				"in the property panel. Peak holds for ~500 ms then decays;\n"
				"RMS is a 50 ms moving average.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FMeter>(); },
			},
			{
				"VoiceAllocator", "Voice Allocator",
				"Polyphonic voice allocator. Consumes NoteOn / NoteOff events from\n"
				"the audio command queue (or directly from FMidiInput) and tracks\n"
				"per-voice gate, frequency, velocity, and note number.\n"
				"Mark downstream synthesis nodes per-voice (right-click → Per-voice)\n"
				"so the compiler clones them per voice in 3E-4.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FVoiceAllocator>(); },
			},
			{
				"SidPlayer", "SID Player",
				"Plays a Commodore 64 .sid (PSID v1/v2) tune via the floooh/chips\n"
				"6502 + SID emulators, and exposes both the synthesised audio\n"
				"and the per-voice / global SID register state as outputs.\n"
				"Use F_Cutoff / V*_Freq / V*_Gate as modulation sources for the\n"
				"rest of the graph, or just listen to the audio out directly.\n"
				"Audio fidelity is chiptune-grade (m6581 is simplified vs reSID).",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FSidPlayer>(); },
			},
		};
		return Registry;
	}

	std::shared_ptr<INode> MakeNodeByTypeName(const std::string& TypeName)
	{
		for (const FNodeRegistration& Reg : GetNodeRegistry())
		{
			if (TypeName == Reg.TypeName)
			{
				return Reg.Make();
			}
		}
		return nullptr;
	}

	// Default INode::Clone — instantiate a fresh node by type name and copy
	// param values across by name. Used by the voice allocator's compile-time
	// per-voice clone step. Non-cloneable nodes (MIDI input, virtual keyboard,
	// output) override this to return nullptr so the compiler can reject a
	// per-voice flag set on them.
	std::shared_ptr<INode> INode::Clone() const
	{
		std::shared_ptr<INode> Cloned = MakeNodeByTypeName(GetTypeName());
		if (!Cloned)
		{
			return nullptr;
		}

		const auto SourceInfos = GetParamInfos();
		const auto TargetInfos = Cloned->GetParamInfos();
		for (uint32_t I = 0; I < SourceInfos.size(); ++I)
		{
			for (uint32_t J = 0; J < TargetInfos.size(); ++J)
			{
				if (SourceInfos[I].Name == TargetInfos[J].Name)
				{
					if (SourceInfos[I].Kind == EParamKind::String)
					{
						Cloned->SetParamString(J, GetParamString(I));
					}
					else
					{
						Cloned->SetParamValue(J, GetParamValue(I));
					}
					break;
				}
			}
		}
		return Cloned;
	}
}
