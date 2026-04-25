#include "ui/NodeRegistry.h"

#include "dsp/Adsr.h"
#include "dsp/Gain.h"
#include "dsp/GateButton.h"
#include "dsp/MidiInput.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"
#include "dsp/Svf.h"
#include "dsp/Vca.h"
#include "dsp/VirtualKeyboard.h"

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
		};
		return Registry;
	}
}
