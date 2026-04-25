// Phase 1: graph-driven synth with a node editor UI.
// Audio callback walks a compiled FAudioGraph snapshot; the UI thread rebuilds
// and atomically publishes a new snapshot whenever the graph is edited.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <miniaudio.h>

#include "dsp/Adsr.h"
#include "dsp/Gain.h"
#include "dsp/Node.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"
#include "dsp/VirtualKeyboard.h"
#include "graph/Graph.h"
#include "ui/Editor.h"
#include "ui/Palette.h"

using namespace NodeSynth;

namespace
{
	struct FAudioState
	{
		std::atomic<std::shared_ptr<FAudioGraph>> Graph{ nullptr };
		std::atomic<double> SampleRate{ 48000.0 };
	};

	void AudioCallback(ma_device* Device, void* Output, const void* /*Input*/, ma_uint32 FrameCount)
	{
		FAudioState* State = static_cast<FAudioState*>(Device->pUserData);
		std::shared_ptr<FAudioGraph> Graph = State->Graph.load();

		float* Samples = static_cast<float*>(Output);
		const ma_uint32 Channels = Device->playback.channels;
		const double SampleRate = State->SampleRate.load(std::memory_order_relaxed);

		ma_uint32 Remaining = FrameCount;
		float* Cursor = Samples;

		while (Remaining > 0)
		{
			const uint32_t Block = (Remaining < BlockSize) ? Remaining : BlockSize;
			FProcessContext Ctx;
			Ctx.BlockSize = Block;
			Ctx.SampleRate = SampleRate;

			const float* OutputBuf = nullptr;
			if (Graph && Graph->OutputNode)
			{
				Graph->Process(Ctx);
				OutputBuf = Graph->OutputNode->GetInputBuffer(0);
			}

			for (uint32_t I = 0; I < Block; ++I)
			{
				const float Sample = OutputBuf ? OutputBuf[I] : 0.0f;
				for (uint32_t C = 0; C < Channels; ++C)
				{
					Cursor[I * Channels + C] = Sample;
				}
			}

			Cursor += Block * Channels;
			Remaining -= Block;
		}
	}

	void GlfwErrorCallback(int Code, const char* Description)
	{
		std::fprintf(stderr, "GLFW error %d: %s\n", Code, Description);
	}

	void SeedDefaultPatch(FGraphModel& Model)
	{
		auto Kbd = std::make_shared<FVirtualKeyboard>();
		auto Adsr = std::make_shared<FAdsr>();
		auto Osc = std::make_shared<FOscillator>();
		auto GainNode = std::make_shared<FGain>();
		auto Out = std::make_shared<FOutput>();

		const FNodeId KbdId = Model.AddNode(Kbd, 60.0f, 60.0f);
		const FNodeId AdsrId = Model.AddNode(Adsr, 60.0f, 240.0f);
		const FNodeId OscId = Model.AddNode(Osc, 340.0f, 120.0f);
		const FNodeId GainId = Model.AddNode(GainNode, 600.0f, 120.0f);
		const FNodeId OutId = Model.AddNode(Out, 860.0f, 120.0f);

		// Keyboard drives oscillator pitch and the envelope gate; envelope drives
		// oscillator amplitude. Press a key in the property panel to hear the patch.
		Model.AddLink(KbdId, FVirtualKeyboard::Output_Frequency, OscId, FOscillator::Input_Frequency);
		Model.AddLink(KbdId, FVirtualKeyboard::Output_Gate, AdsrId, 0);
		Model.AddLink(AdsrId, 0, OscId, FOscillator::Input_Amplitude);
		Model.AddLink(OscId, 0, GainId, 0);
		Model.AddLink(GainId, 0, OutId, 0);
	}
}

int main()
{
	// ---- GLFW + OpenGL context ---------------------------------------------
	glfwSetErrorCallback(GlfwErrorCallback);
	if (!glfwInit())
	{
		std::fprintf(stderr, "glfwInit failed\n");
		return 1;
	}

	const char* GlslVersion = "#version 150";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

	GLFWwindow* Window = glfwCreateWindow(1440, 900, "NodeSynth — Phase 1", nullptr, nullptr);
	if (!Window)
	{
		std::fprintf(stderr, "glfwCreateWindow failed\n");
		glfwTerminate();
		return 1;
	}
	glfwMakeContextCurrent(Window);
	glfwSwapInterval(1);

	// ---- Dear ImGui ---------------------------------------------------------
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& IO = ImGui::GetIO();
	IO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
#ifdef __APPLE__
	IO.ConfigMacOSXBehaviors = true;
#endif
	ImGui::StyleColorsDark();
	ImGui_ImplGlfw_InitForOpenGL(Window, true);
	ImGui_ImplOpenGL3_Init(GlslVersion);

	// ---- Graph + editor -----------------------------------------------------
	FGraphModel Model;
	FGraphEditorPanel EditorPanel;
	SeedDefaultPatch(Model);

	FAudioState AudioState;
	AudioState.Graph.store(Model.Compile(48000.0));

	// ---- miniaudio ----------------------------------------------------------
	ma_device_config Config = ma_device_config_init(ma_device_type_playback);
	Config.playback.format = ma_format_f32;
	Config.playback.channels = 2;
	Config.sampleRate = 0;
	Config.dataCallback = AudioCallback;
	Config.pUserData = &AudioState;

	ma_device Device;
	if (ma_device_init(nullptr, &Config, &Device) != MA_SUCCESS)
	{
		std::fprintf(stderr, "ma_device_init failed\n");
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		glfwDestroyWindow(Window);
		glfwTerminate();
		return 1;
	}

	AudioState.SampleRate.store(static_cast<double>(Device.sampleRate));
	AudioState.Graph.store(Model.Compile(static_cast<double>(Device.sampleRate)));

	if (ma_device_start(&Device) != MA_SUCCESS)
	{
		std::fprintf(stderr, "ma_device_start failed\n");
	}

	// ---- Main loop ----------------------------------------------------------
	while (!glfwWindowShouldClose(Window))
	{
		glfwPollEvents();

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();

		// Build a root dockspace so the two panels tile nicely.
		const ImGuiViewport* Viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(Viewport->WorkPos);
		ImGui::SetNextWindowSize(Viewport->WorkSize);
		ImGui::SetNextWindowViewport(Viewport->ID);
		ImGuiWindowFlags HostFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar
			| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
			| ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus
			| ImGuiWindowFlags_NoBackground;
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("##DockspaceHost", nullptr, HostFlags);
		ImGui::PopStyleVar(3);
		ImGui::DockSpace(ImGui::GetID("RootDockSpace"), ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
		ImGui::End();

		// Node editor
		ImGui::Begin("Graph");
		const bool bGraphChanged = EditorPanel.Draw(Model);
		ImGui::End();

		// Property panel
		ImGui::Begin("Properties");
		EditorPanel.DrawPropertyPanel(Model);
		ImGui::End();

		// Keyboard panel (always visible so notes can be played while editing
		// other nodes' parameters).
		ImGui::Begin("Keyboard");
		EditorPanel.DrawKeyboardPanel(Model);
		ImGui::End();

		// Node palette — drag entries onto the Graph window to create nodes.
		ImGui::Begin("Palette");
		DrawNodePalette();
		ImGui::End();

		// Status
		ImGui::Begin("Audio");
		ImGui::Text("Sample rate: %.0f Hz", AudioState.SampleRate.load());
		ImGui::Text("Backend:     %s", ma_get_backend_name(Device.pContext->backend));
		ImGui::Text("Block size:  %u samples", BlockSize);
		ImGui::Text("Nodes:       %zu", Model.GetNodes().size());
		ImGui::Text("Links:       %zu", Model.GetLinks().size());
		ImGui::End();

		if (bGraphChanged)
		{
			AudioState.Graph.store(Model.Compile(AudioState.SampleRate.load()));
		}

		ImGui::Render();
		int Width = 0;
		int Height = 0;
		glfwGetFramebufferSize(Window, &Width, &Height);
		glViewport(0, 0, Width, Height);
		glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		glfwSwapBuffers(Window);
	}

	// ---- Shutdown -----------------------------------------------------------
	ma_device_uninit(&Device);

	// Drop the current graph before ImGui shutdown so any node destructors run
	// on the UI thread (and well before the audio thread is gone).
	AudioState.Graph.store(nullptr);

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	glfwDestroyWindow(Window);
	glfwTerminate();
	return 0;
}
