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

namespace {

struct AudioState {
    // Read/written by the audio thread; read atomically by the UI thread for display only.
    double phase        = 0.0;
    double sampleRate   = 0.0;
    std::atomic<float> frequency{440.0f};
    std::atomic<float> amplitude{0.15f};
    std::atomic<bool>  muted{false};
};

void audioCallback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frameCount) {
    auto* state   = static_cast<AudioState*>(device->pUserData);
    auto* samples = static_cast<float*>(output);

    const ma_uint32 channels = device->playback.channels;
    const float     freq     = state->frequency.load(std::memory_order_relaxed);
    const float     amp      = state->muted.load(std::memory_order_relaxed)
                                   ? 0.0f
                                   : state->amplitude.load(std::memory_order_relaxed);

    const double twoPi    = std::numbers::pi * 2.0;
    const double phaseInc = twoPi * static_cast<double>(freq) / state->sampleRate;

    for (ma_uint32 i = 0; i < frameCount; ++i) {
        const float s = amp * static_cast<float>(std::sin(state->phase));
        for (ma_uint32 c = 0; c < channels; ++c) {
            samples[i * channels + c] = s;
        }
        state->phase += phaseInc;
        if (state->phase >= twoPi) state->phase -= twoPi;
    }
}

void glfwErrorCallback(int code, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", code, description);
}

} // namespace

int main() {
    // ---- GLFW + OpenGL context ---------------------------------------------
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }

    // OpenGL 3.2 Core — works on macOS (where it's the ceiling) and Windows.
    const char* glslVersion = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(1280, 800, "NodeSynth — Phase 0", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync

    // ---- Dear ImGui ---------------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
#ifdef __APPLE__
    io.ConfigMacOSXBehaviors = true;
#endif
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    // ---- miniaudio ----------------------------------------------------------
    AudioState audio;

    ma_device_config config     = ma_device_config_init(ma_device_type_playback);
    config.playback.format      = ma_format_f32;
    config.playback.channels    = 2;
    config.sampleRate           = 0; // native rate
    config.dataCallback         = audioCallback;
    config.pUserData            = &audio;

    ma_device device;
    if (ma_device_init(nullptr, &config, &device) != MA_SUCCESS) {
        std::fprintf(stderr, "ma_device_init failed\n");
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    audio.sampleRate = static_cast<double>(device.sampleRate);

    if (ma_device_start(&device) != MA_SUCCESS) {
        std::fprintf(stderr, "ma_device_start failed\n");
    }

    // ---- Main loop ----------------------------------------------------------
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("NodeSynth — Phase 0");
        ImGui::Text("Sample rate: %.0f Hz", audio.sampleRate);
        ImGui::Text("Backend:     %s", ma_get_backend_name(device.pContext->backend));

        float freq = audio.frequency.load(std::memory_order_relaxed);
        if (ImGui::SliderFloat("Frequency (Hz)", &freq, 20.0f, 2000.0f, "%.1f", ImGuiSliderFlags_Logarithmic)) {
            audio.frequency.store(freq, std::memory_order_relaxed);
        }

        float amp = audio.amplitude.load(std::memory_order_relaxed);
        if (ImGui::SliderFloat("Amplitude", &amp, 0.0f, 1.0f)) {
            audio.amplitude.store(amp, std::memory_order_relaxed);
        }

        bool muted = audio.muted.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Mute", &muted)) {
            audio.muted.store(muted, std::memory_order_relaxed);
        }
        ImGui::End();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // ---- Shutdown -----------------------------------------------------------
    ma_device_uninit(&device);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
