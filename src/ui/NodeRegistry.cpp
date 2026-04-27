#include "ui/NodeRegistry.h"

#include <cstring>

#include "dsp/Add.h"
#include "dsp/Adsr.h"
#include "dsp/Constant.h"
#include "dsp/Delay.h"
#include "dsp/Gain.h"
#include "dsp/GateButton.h"
#include "dsp/Lfo.h"
#include "dsp/MidiInput.h"
#include "dsp/Multiply.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"
#include "dsp/SampleHold.h"
#include "dsp/Scale.h"
#include "dsp/Svf.h"
#include "dsp/Vca.h"
#include "dsp/VirtualKeyboard.h"
#include "dsp/VoiceAllocator.h"

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
				"MIDI", "MIDI Input",
				"Hardware MIDI input via RtMidi.\n"
				"Outputs: Gate, Frequency (Hz), Velocity (0..1).\n"
				"Monophonic, last-note-wins with legato.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FMidiInput>(); },
			},
			{
				"VirtualKbd", "Virtual Keyboard",
				"On-screen keyboard. Click keys with the mouse or use A S D F G H J K\n"
				"(white) and W E T Y U (black) on the computer keyboard.\n"
				"Outputs: Gate, Frequency, Velocity, ModWheel.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FVirtualKeyboard>(); },
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
				"VoiceAllocator", "Voice Allocator",
				"Polyphonic voice allocator. Consumes NoteOn / NoteOff events from\n"
				"the audio command queue (or directly from FMidiInput) and tracks\n"
				"per-voice gate, frequency, velocity, and note number.\n"
				"Mark downstream synthesis nodes per-voice (right-click → Per-voice)\n"
				"so the compiler clones them per voice in 3E-4.",
				[]() -> std::shared_ptr<INode> { return std::make_shared<FVoiceAllocator>(); },
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
					Cloned->SetParamValue(J, GetParamValue(I));
					break;
				}
			}
		}
		return Cloned;
	}
}
