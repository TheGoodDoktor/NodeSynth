// Phase 1: graph-driven synth with a node editor UI.
// Audio callback walks a compiled FAudioGraph snapshot; the UI thread rebuilds
// and atomically publishes a new snapshot whenever the graph is edited.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <miniaudio.h>
#include <nfd.h>

#include "dsp/Adsr.h"
#include "dsp/Gain.h"
#include "dsp/Meter.h"
#include "dsp/Node.h"
#include "dsp/Oscillator.h"
#include "dsp/Output.h"
#include "dsp/VirtualKeyboard.h"
#include "dsp/VoiceAllocator.h"
#include "graph/EditHistory.h"
#include "graph/Graph.h"
#include "io/PatchSerializer.h"
#include "io/PresetBrowser.h"
#include "ui/Editor.h"
#include "ui/Palette.h"

using namespace NodeSynth;

namespace
{
	struct FAudioState
	{
		std::atomic<std::shared_ptr<FAudioGraph>> Graph{ nullptr };
		std::atomic<double> SampleRate{ 48000.0 };
		FAudioCommandRing Commands;
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
				Graph->DrainCommands(State->Commands);
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
		auto Alloc = std::make_shared<FVoiceAllocator>();
		auto Adsr = std::make_shared<FAdsr>();
		auto Osc = std::make_shared<FOscillator>();
		auto GainNode = std::make_shared<FGain>();
		auto MeterNode = std::make_shared<FMeter>();
		auto Out = std::make_shared<FOutput>();

		// Master gain trimmed to ~1/8 so the 8-voice sum stays inside [-1, 1]
		// even at sustain. The Oscillator's Amplitude param is overridden by
		// the connected ADSR.Env, so attenuation has to live downstream of the
		// per-voice mixer — this Gain node is exactly that.
		GainNode->SetParamValue(FGain::Param_Gain, 0.15f);

		Model.AddNode(Kbd, 60.0f, 60.0f);
		const FNodeId AllocId = Model.AddNode(Alloc, 60.0f, 240.0f);
		const FNodeId AdsrId = Model.AddNode(Adsr, 340.0f, 60.0f);
		const FNodeId OscId = Model.AddNode(Osc, 340.0f, 240.0f);
		const FNodeId GainId = Model.AddNode(GainNode, 620.0f, 180.0f);
		const FNodeId MeterId = Model.AddNode(MeterNode, 860.0f, 180.0f);
		const FNodeId OutId = Model.AddNode(Out, 1100.0f, 180.0f);

		// Mark the synthesis nodes per-voice so the compiler clones them ×8.
		// The keyboard pushes NoteOn/NoteOff into the audio command queue (no
		// graph cable to the allocator); the allocator drains them and emits
		// per-voice gate / frequency / velocity buffers.
		Model.SetNodePerVoice(AdsrId, true);
		Model.SetNodePerVoice(OscId, true);

		Model.AddLink(AllocId, FVoiceAllocator::Output_Gate, AdsrId, 0);
		Model.AddLink(AllocId, FVoiceAllocator::Output_Frequency, OscId, FOscillator::Input_Frequency);
		Model.AddLink(AdsrId, 0, OscId, FOscillator::Input_Amplitude);
		Model.AddLink(OscId, 0, GainId, 0);  // per-voice → mono Audio: synthesised mixer
		Model.AddLink(GainId, 0, MeterId, 0);
		Model.AddLink(MeterId, 0, OutId, 0);
	}

	// Returns the per-user settings directory (~/.nodesynth/, %USERPROFILE%\.nodesynth\
	// on Windows). Falls back to the current directory if the home env var is unset.
	std::filesystem::path GetSettingsDir()
	{
#ifdef _WIN32
		const char* HomeVar = "USERPROFILE";
#else
		const char* HomeVar = "HOME";
#endif
		// MSVC flags std::getenv as "unsafe"; the underlying string is owned by
		// the runtime and we never write to it, so this is fine — silence the
		// warning locally rather than suppressing project-wide.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
		const char* Home = std::getenv(HomeVar);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
		std::filesystem::path Dir = Home
			? std::filesystem::path(Home) / ".nodesynth"
			: std::filesystem::path(".");
		std::error_code Ec;
		std::filesystem::create_directories(Dir, Ec);
		return Dir;
	}

	// Native open/save dialog wrappers. Return std::nullopt on cancel or error;
	// callers fall back to the typed-path popup so a missing OS dialog (rare,
	// sandboxed environments) doesn't lock the user out.
	std::optional<std::filesystem::path> OpenFileDialogNative()
	{
		nfdu8char_t* OutPath = nullptr;
		nfdu8filteritem_t Filter[1] = { { "NodeSynth Patch", "json" } };
		nfdopendialogu8args_t Args = {};
		Args.filterList = Filter;
		Args.filterCount = 1;
		const nfdresult_t Result = NFD_OpenDialogU8_With(&OutPath, &Args);
		if (Result == NFD_OKAY)
		{
			std::filesystem::path P(OutPath);
			NFD_FreePathU8(OutPath);
			return P;
		}
		return std::nullopt;
	}

	std::optional<std::filesystem::path> SaveFileDialogNative(const std::filesystem::path& Default)
	{
		nfdu8char_t* OutPath = nullptr;
		nfdu8filteritem_t Filter[1] = { { "NodeSynth Patch", "json" } };
		nfdsavedialogu8args_t Args = {};
		Args.filterList = Filter;
		Args.filterCount = 1;
		const std::string DefaultStr = Default.empty() ? std::string() : Default.string();
		if (!DefaultStr.empty())
		{
			Args.defaultName = DefaultStr.c_str();
		}
		const nfdresult_t Result = NFD_SaveDialogU8_With(&OutPath, &Args);
		if (Result == NFD_OKAY)
		{
			std::filesystem::path P(OutPath);
			NFD_FreePathU8(OutPath);
			return P;
		}
		return std::nullopt;
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

	// ---- Settings directory + persistent layout files ---------------------
	const std::filesystem::path SettingsDir = GetSettingsDir();
	// These paths must outlive the ImGui / node-editor contexts since both
	// libraries store the const char* and read it across the session.
	const std::string ImGuiIniPath = (SettingsDir / "imgui.ini").string();
	const std::string NodeEditorIniPath = (SettingsDir / "node_editor.ini").string();

	// ---- DPI scale ---------------------------------------------------------
	// GLFW reports the per-monitor content scale; on Windows / macOS Retina
	// displays this is typically 1.5–2.0. We scale the font and the ImGui style
	// up by the same factor so the whole UI grows proportionally instead of
	// becoming illegibly small. Dynamic re-scaling on monitor switch is not
	// handled — the scale is locked at the value reported when the window first
	// appears.
	float DpiX = 1.0f;
	float DpiY = 1.0f;
	glfwGetWindowContentScale(Window, &DpiX, &DpiY);
	const float DpiScale = std::max(1.0f, std::max(DpiX, DpiY));

	// ---- Dear ImGui ---------------------------------------------------------
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& IO = ImGui::GetIO();
	IO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	IO.IniFilename = ImGuiIniPath.c_str();
#ifdef __APPLE__
	IO.ConfigMacOSXBehaviors = true;
#endif

	// Load the default font (ProggyClean) at the DPI-scaled pixel size so text
	// stays sharp rather than getting bilinearly upscaled.
	{
		ImFontConfig FontCfg;
		FontCfg.SizePixels = 13.0f * DpiScale;
		IO.Fonts->AddFontDefault(&FontCfg);
	}

	ImGui::StyleColorsDark();
	if (DpiScale != 1.0f)
	{
		// Scale frame padding, item spacing, scrollbar widths, etc. so the whole
		// chrome matches the larger font. Call exactly once.
		ImGui::GetStyle().ScaleAllSizes(DpiScale);
	}

	ImGui_ImplGlfw_InitForOpenGL(Window, true);
	ImGui_ImplOpenGL3_Init(GlslVersion);

	// ---- Native file dialog ------------------------------------------------
	const bool bNfdReady = (NFD_Init() == NFD_OKAY);
	if (!bNfdReady)
	{
		std::fprintf(stderr, "NFD_Init failed: %s\n", NFD_GetError());
	}

	// ---- Graph + editor -----------------------------------------------------
	FGraphModel Model;
	FEditHistory EditHistory;
	// SeedDefaultPatch builds the graph BEFORE we attach the history, so the
	// initial state isn't undoable (good — the user can't undo back to an
	// empty graph that's never been seen).
	SeedDefaultPatch(Model);
	Model.SetHistory(&EditHistory);

	FGraphEditorPanel EditorPanel(NodeEditorIniPath);
	EditorPanel.SetEditHistory(&EditHistory);

	// File-menu state.
	std::filesystem::path CurrentPatchPath;
	std::string PathInputBuffer(512, '\0');
	bool bOpenSavePopup = false;
	bool bOpenLoadPopup = false;
	std::string PopupErrorMsg;

	// Preset menu: bundled presets shipped next to the binary, plus user
	// presets in ~/.nodesynth/user_presets/. Scanned at startup; a "Refresh"
	// item rebuilds the index without restarting the app.
	const std::filesystem::path BundledPresetDir = GetBundledPresetDir();
	const std::filesystem::path UserPresetDir = GetSettingsDir() / "user_presets";
	FPresetIndex PresetIndex = BuildPresetIndex(BundledPresetDir, UserPresetDir);
	std::filesystem::path PendingPresetLoad;

	FAudioState AudioState;
	AudioState.Graph.store(Model.Compile(48000.0));
	EditorPanel.SetCommandRing(&AudioState.Commands);

	// Last load/save warning surfaced to the UI (e.g. sample-rate mismatch).
	// Cleared on successful action; rendered as a non-blocking notice.
	std::string LastIoNotice;

	// Apply a load. Returns true on success, sets CurrentPatchPath, etc.
	// Defined after AudioState because the closures capture it.
	auto DoLoadPatch = [&](const std::filesystem::path& P) -> bool
	{
		auto Loaded = LoadPatch(P);
		if (!Loaded)
		{
			return false;
		}
		// Compare the patch's saved sample rate to the live device rate;
		// surface a warning if they differ. The patch still loads — sample-rate
		// dependent state (delay buffers, smoother coefficients) is recomputed
		// on Compile via Prepare().
		const double LoadedHint = Loaded->Model.GetMetadata().SampleRateHint;
		const double DeviceRate = AudioState.SampleRate.load();
		LastIoNotice.clear();
		if (LoadedHint > 0.0 && DeviceRate > 0.0
			&& std::fabs(LoadedHint - DeviceRate) > 0.5)
		{
			LastIoNotice = "Patch was saved at "
				+ std::to_string(static_cast<int>(LoadedHint))
				+ " Hz but the device is running at "
				+ std::to_string(static_cast<int>(DeviceRate))
				+ " Hz. Time-based effects may sound slightly off.";
		}
		Model = std::move(Loaded->Model);
		Model.SetHistory(&EditHistory);
		EditHistory.Clear();  // loaded patch is the new ground state
		EditorPanel.OnModelReplaced();
		CurrentPatchPath = P;
		auto NewSnapshot = Model.Compile(AudioState.SampleRate.load());
		if (!Model.GetLastCompileError().bHasError)
		{
			AudioState.Graph.store(std::move(NewSnapshot));
		}
		for (const auto& Cmd : Loaded->InitialParams)
		{
			AudioState.Commands.Push(Cmd);
		}
		return true;
	};

	auto DoSavePatch = [&](const std::filesystem::path& P) -> bool
	{
		if (P.empty())
		{
			return false;
		}
		// Stamp the current device rate into the metadata so future loads can
		// detect mismatch.
		Model.GetMetadata().SampleRateHint = AudioState.SampleRate.load();
		if (!SavePatch(Model, P))
		{
			return false;
		}
		CurrentPatchPath = P;
		LastIoNotice.clear();
		return true;
	};

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

		// ---- File / Edit menu -------------------------------------------------
		bool bRequestNew = false;
		bool bRequestSave = false;
		bool bRequestUndo = false;
		bool bRequestRedo = false;
		if (ImGui::BeginMainMenuBar())
		{
			if (ImGui::BeginMenu("File"))
			{
				if (ImGui::MenuItem("New"))
				{
					bRequestNew = true;
				}
				if (ImGui::MenuItem("Open..."))
				{
					if (bNfdReady)
					{
						if (auto P = OpenFileDialogNative())
						{
							if (!DoLoadPatch(*P))
							{
								std::fprintf(stderr, "Load failed for %s\n", P->string().c_str());
							}
						}
					}
					else
					{
						bOpenLoadPopup = true;
					}
				}
				if (ImGui::BeginMenu("Presets", !PresetIndex.IsEmpty()))
				{
					for (const FPresetCategory& Cat : PresetIndex.Categories)
					{
						const bool bRoot = Cat.Name.empty();
						const bool bOpenSub = bRoot ? true : ImGui::BeginMenu(Cat.Name.c_str());
						if (bOpenSub)
						{
							for (const FPresetEntry& E : Cat.Entries)
							{
								if (ImGui::MenuItem(E.DisplayName.c_str()))
								{
									PendingPresetLoad = E.FullPath;
								}
							}
							if (!bRoot) { ImGui::EndMenu(); }
						}
					}
					ImGui::Separator();
					if (ImGui::MenuItem("Refresh"))
					{
						PresetIndex = BuildPresetIndex(BundledPresetDir, UserPresetDir);
					}
					ImGui::EndMenu();
				}
				if (ImGui::MenuItem("Save", nullptr, false, !CurrentPatchPath.empty()))
				{
					bRequestSave = true;
				}
				if (ImGui::MenuItem("Save As..."))
				{
					if (bNfdReady)
					{
						if (auto P = SaveFileDialogNative(CurrentPatchPath))
						{
							// nfd-extended doesn't auto-add the filter extension
							// on every platform — make sure .json is on the path.
							std::filesystem::path Path = *P;
							if (Path.extension().empty())
							{
								Path += ".json";
							}
							if (!DoSavePatch(Path))
							{
								std::fprintf(stderr, "Save failed for %s\n", Path.string().c_str());
							}
						}
					}
					else
					{
						bOpenSavePopup = true;
					}
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Edit"))
			{
				if (ImGui::MenuItem("Undo", "Ctrl+Z", false, EditHistory.CanUndo()))
				{
					bRequestUndo = true;
				}
				if (ImGui::MenuItem("Redo", "Ctrl+Y", false, EditHistory.CanRedo()))
				{
					bRequestRedo = true;
				}
				ImGui::EndMenu();
			}
			if (!CurrentPatchPath.empty())
			{
				const std::string Label = CurrentPatchPath.filename().string();
				ImGui::TextDisabled("  [%s]", Label.c_str());
			}
			ImGui::EndMainMenuBar();
		}

		// Apply a pending preset load deferred from inside the menu loop, so
		// the menu has fully torn down before Model / EditHistory mutate.
		if (!PendingPresetLoad.empty())
		{
			if (!DoLoadPatch(PendingPresetLoad))
			{
				std::fprintf(stderr, "Preset load failed for %s\n",
					PendingPresetLoad.string().c_str());
			}
			PendingPresetLoad.clear();
		}

		// Hotkeys: Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z. Gated on no active text input
		// so they don't fire while typing in a path popup.
		{
			const ImGuiIO& IO = ImGui::GetIO();
			const bool bCtrl = IO.KeyCtrl || IO.KeySuper;  // Cmd on macOS
			if (!IO.WantTextInput && bCtrl)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_Z, false) && !IO.KeyShift)
				{
					bRequestUndo = true;
				}
				if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
				{
					bRequestRedo = true;
				}
				if (ImGui::IsKeyPressed(ImGuiKey_Z, false) && IO.KeyShift)
				{
					bRequestRedo = true;
				}
			}
		}

		if (bOpenSavePopup)
		{
			ImGui::OpenPopup("Save Patch");
			bOpenSavePopup = false;
			if (!CurrentPatchPath.empty())
			{
				const std::string Cur = CurrentPatchPath.string();
				const size_t Copy = std::min(Cur.size(), PathInputBuffer.size() - 1);
				std::memcpy(PathInputBuffer.data(), Cur.data(), Copy);
				PathInputBuffer[Copy] = '\0';
			}
			PopupErrorMsg.clear();
		}
		if (bOpenLoadPopup)
		{
			ImGui::OpenPopup("Open Patch");
			bOpenLoadPopup = false;
			PopupErrorMsg.clear();
		}

		if (ImGui::BeginPopupModal("Save Patch", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextUnformatted("Path:");
			ImGui::SetNextItemWidth(420.0f);
			ImGui::InputText("##save_path", PathInputBuffer.data(), PathInputBuffer.size());
			if (!PopupErrorMsg.empty())
			{
				ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", PopupErrorMsg.c_str());
			}
			if (ImGui::Button("Save"))
			{
				const std::filesystem::path P(PathInputBuffer.c_str());
				if (P.empty())
				{
					PopupErrorMsg = "Path is empty.";
				}
				else if (DoSavePatch(P))
				{
					ImGui::CloseCurrentPopup();
				}
				else
				{
					PopupErrorMsg = "Save failed (see stderr).";
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal("Open Patch", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextUnformatted("Path:");
			ImGui::SetNextItemWidth(420.0f);
			ImGui::InputText("##load_path", PathInputBuffer.data(), PathInputBuffer.size());
			if (!PopupErrorMsg.empty())
			{
				ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", PopupErrorMsg.c_str());
			}
			if (ImGui::Button("Open"))
			{
				const std::filesystem::path P(PathInputBuffer.c_str());
				if (DoLoadPatch(P))
				{
					ImGui::CloseCurrentPopup();
				}
				else
				{
					PopupErrorMsg = "Load failed (see stderr).";
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		if (bRequestNew)
		{
			Model = FGraphModel{};
			Model.SetHistory(&EditHistory);
			EditHistory.Clear();
			// Seed the new model with an Output before re-attaching history,
			// so the initial Output isn't undoable.
			Model.SetRecordHistory(false);
			Model.AddNode(std::make_shared<FOutput>(), 600.0f, 200.0f);
			Model.SetRecordHistory(true);
			EditorPanel.OnModelReplaced();
			CurrentPatchPath.clear();
			AudioState.Graph.store(Model.Compile(AudioState.SampleRate.load()));
		}
		if (bRequestSave && !CurrentPatchPath.empty())
		{
			DoSavePatch(CurrentPatchPath);
		}
		if (bRequestUndo && EditHistory.Undo(Model))
		{
			EditorPanel.OnModelReplaced();
			auto NewSnapshot = Model.Compile(AudioState.SampleRate.load());
			if (!Model.GetLastCompileError().bHasError)
			{
				AudioState.Graph.store(std::move(NewSnapshot));
			}
		}
		if (bRequestRedo && EditHistory.Redo(Model))
		{
			EditorPanel.OnModelReplaced();
			auto NewSnapshot = Model.Compile(AudioState.SampleRate.load());
			if (!Model.GetLastCompileError().bHasError)
			{
				AudioState.Graph.store(std::move(NewSnapshot));
			}
		}

		// History-panel jump request. Positive = Undo N, negative = Redo N.
		// Single recompile after the whole jump finishes.
		if (const int32_t Jump = EditorPanel.TakePendingHistoryJump(); Jump != 0)
		{
			bool bChanged = false;
			if (Jump > 0)
			{
				for (int32_t I = 0; I < Jump; ++I)
				{
					if (EditHistory.Undo(Model)) { bChanged = true; }
					else { break; }
				}
			}
			else
			{
				for (int32_t I = 0; I < -Jump; ++I)
				{
					if (EditHistory.Redo(Model)) { bChanged = true; }
					else { break; }
				}
			}
			if (bChanged)
			{
				EditorPanel.OnModelReplaced();
				auto NewSnapshot = Model.Compile(AudioState.SampleRate.load());
				if (!Model.GetLastCompileError().bHasError)
				{
					AudioState.Graph.store(std::move(NewSnapshot));
				}
			}
		}

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
		if (!LastIoNotice.empty())
		{
			ImGui::Separator();
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.4f, 1.0f));
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(LastIoNotice.c_str());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
			if (ImGui::SmallButton("Dismiss"))
			{
				LastIoNotice.clear();
			}
		}
		ImGui::End();

		ImGui::Begin("Patch Info");
		EditorPanel.DrawPatchInfoPanel(Model);
		ImGui::End();

		ImGui::Begin("History");
		EditorPanel.DrawHistoryPanel(Model);
		ImGui::End();

		if (bGraphChanged)
		{
			auto NewSnapshot = Model.Compile(AudioState.SampleRate.load());
			if (!Model.GetLastCompileError().bHasError)
			{
				AudioState.Graph.store(std::move(NewSnapshot));
			}
			// On compile failure, keep the previous good snapshot live so the
			// audio doesn't go silent. The error is surfaced in the UI.
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

	if (bNfdReady)
	{
		NFD_Quit();
	}

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	glfwDestroyWindow(Window);
	glfwTerminate();
	return 0;
}
