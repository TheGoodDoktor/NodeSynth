// Phase 0: window + ImGui + sine-from-callback.
// Intentionally all in one file — abstractions arrive with Phase 1.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <numbers>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <miniaudio.h>

namespace
{
	struct FAudioState
	{
		// Phase is read and written by the audio thread only.
		// Other fields are written by the UI thread and read by the audio thread.
		double Phase = 0.0;
		double SampleRate = 0.0;
		std::atomic<float> Frequency{ 440.0f };
		std::atomic<float> Amplitude{ 0.15f };
		std::atomic<bool> bMuted{ false };
	};

	void AudioCallback(ma_device* Device, void* Output, const void* /*Input*/, ma_uint32 FrameCount)
	{
		FAudioState* State = static_cast<FAudioState*>(Device->pUserData);
		float* Samples = static_cast<float*>(Output);

		const ma_uint32 Channels = Device->playback.channels;
		const float Freq = State->Frequency.load(std::memory_order_relaxed);
		const float Amp = State->bMuted.load(std::memory_order_relaxed)
			? 0.0f
			: State->Amplitude.load(std::memory_order_relaxed);

		const double TwoPi = std::numbers::pi * 2.0;
		const double PhaseInc = TwoPi * static_cast<double>(Freq) / State->SampleRate;

		for (ma_uint32 FrameIndex = 0; FrameIndex < FrameCount; ++FrameIndex)
		{
			const float Sample = Amp * static_cast<float>(std::sin(State->Phase));
			for (ma_uint32 ChannelIndex = 0; ChannelIndex < Channels; ++ChannelIndex)
			{
				Samples[FrameIndex * Channels + ChannelIndex] = Sample;
			}
			State->Phase += PhaseInc;
			if (State->Phase >= TwoPi)
			{
				State->Phase -= TwoPi;
			}
		}
	}

	void GlfwErrorCallback(int Code, const char* Description)
	{
		std::fprintf(stderr, "GLFW error %d: %s\n", Code, Description);
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

	// OpenGL 3.2 Core — works on macOS (where it's the ceiling) and Windows.
	const char* GlslVersion = "#version 150";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

	GLFWwindow* Window = glfwCreateWindow(1280, 800, "NodeSynth — Phase 0", nullptr, nullptr);
	if (!Window)
	{
		std::fprintf(stderr, "glfwCreateWindow failed\n");
		glfwTerminate();
		return 1;
	}
	glfwMakeContextCurrent(Window);
	glfwSwapInterval(1); // vsync

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

	// ---- miniaudio ----------------------------------------------------------
	FAudioState Audio;

	ma_device_config Config = ma_device_config_init(ma_device_type_playback);
	Config.playback.format = ma_format_f32;
	Config.playback.channels = 2;
	Config.sampleRate = 0; // native rate
	Config.dataCallback = AudioCallback;
	Config.pUserData = &Audio;

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
	Audio.SampleRate = static_cast<double>(Device.sampleRate);

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

		ImGui::Begin("NodeSynth — Phase 0");
		ImGui::Text("Sample rate: %.0f Hz", Audio.SampleRate);
		ImGui::Text("Backend:     %s", ma_get_backend_name(Device.pContext->backend));

		float Freq = Audio.Frequency.load(std::memory_order_relaxed);
		if (ImGui::SliderFloat("Frequency (Hz)", &Freq, 20.0f, 2000.0f, "%.1f", ImGuiSliderFlags_Logarithmic))
		{
			Audio.Frequency.store(Freq, std::memory_order_relaxed);
		}

		float Amp = Audio.Amplitude.load(std::memory_order_relaxed);
		if (ImGui::SliderFloat("Amplitude", &Amp, 0.0f, 1.0f))
		{
			Audio.Amplitude.store(Amp, std::memory_order_relaxed);
		}

		bool bMuted = Audio.bMuted.load(std::memory_order_relaxed);
		if (ImGui::Checkbox("Mute", &bMuted))
		{
			Audio.bMuted.store(bMuted, std::memory_order_relaxed);
		}
		ImGui::End();

		ImGui::Render();
		int Width;
		int Height;
		glfwGetFramebufferSize(Window, &Width, &Height);
		glViewport(0, 0, Width, Height);
		glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		glfwSwapBuffers(Window);
	}

	// ---- Shutdown -----------------------------------------------------------
	ma_device_uninit(&Device);

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	glfwDestroyWindow(Window);
	glfwTerminate();
	return 0;
}
