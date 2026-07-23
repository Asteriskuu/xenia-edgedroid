#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <thread>
#include <memory>
#include <string>
#include <filesystem>
#include <android/log.h>

#include "xenia/emulator.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/apu/nop/nop_audio_system.h"
#include "xenia/hid/nop/nop_input_driver.h"
#include "xenia/config.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

#define LOG_TAG "XeniaAndroid"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static std::unique_ptr<xe::Emulator> g_emulator;
static std::thread g_emulator_thread;
static bool g_emulator_running = false;
static ANativeWindow* g_native_window = nullptr;

class AndroidAppContext : public xe::ui::WindowedAppContext {
public:
    void NotifyUILoopOfPendingFunctions() override {}
    void PlatformQuitFromUIThread() override {}
};
static AndroidAppContext g_app_context;

class AndroidDisplayWindow : public xe::ui::Window {
public:
    AndroidDisplayWindow(xe::ui::WindowedAppContext& context)
        : xe::ui::Window(context, "Xenia", 1920, 1080) {}

    ~AndroidDisplayWindow() override = default;

    uintptr_t native_handle() const {
        return reinterpret_cast<uintptr_t>(g_native_window);
    }

    bool Initialize() { return true; }
    void OnClose() {}
};
static std::unique_ptr<AndroidDisplayWindow> g_display_window;

extern "C" {

JNIEXPORT void JNICALL
Java_jp_xenia_emulator_WindowDemoActivity_nativeBootGame(
    JNIEnv* env, jobject thiz,
    jstring jgame_path, jobject surface) {

    const char* game_path = env->GetStringUTFChars(jgame_path, nullptr);
    ANativeWindow* native_window = ANativeWindow_fromSurface(env, surface);

    if (!native_window) {
        LOGE("Failed to get native window");
        env->ReleaseStringUTFChars(jgame_path, game_path);
        return;
    }

    LOGI("Booting game: %s", game_path);

    g_native_window = native_window;
    g_emulator_running = true;

    g_emulator_thread = std::thread([game_path = std::string(game_path), native_window]() {
        try {
            std::filesystem::path storage_root = "/data/data/jp.xenia.emulator";
            std::filesystem::create_directories(storage_root / "content");
            std::filesystem::create_directories(storage_root / "cache");

            LOGI("Creating emulator with path: %s", game_path.c_str());

            g_emulator = std::make_unique<xe::Emulator>(
                game_path,
                storage_root,
                storage_root / "content",
                storage_root / "cache"
            );

            LOGI("Loading game config...");
            config::LoadGameConfigForFile(game_path);

            auto graphics_factory = []() -> std::unique_ptr<xe::gpu::GraphicsSystem> {
                LOGI("Creating Vulkan graphics system");
                return std::make_unique<xe::gpu::vulkan::VulkanGraphicsSystem>();
            };

            auto audio_factory = [](xe::cpu::Processor* processor) -> std::unique_ptr<xe::apu::AudioSystem> {
                LOGI("Creating NOP audio system");
                return std::make_unique<xe::apu::nop::NopAudioSystem>(processor);
            };

            auto input_factory = [](xe::ui::Window* window) -> std::vector<std::unique_ptr<xe::hid::InputDriver>> {
                std::vector<std::unique_ptr<xe::hid::InputDriver>> drivers;
                LOGI("Creating NOP input driver");
                drivers.push_back(std::make_unique<xe::hid::nop::NopInputDriver>(window, 0));
                return drivers;
            };

            LOGI("Setting up emulator");

            g_display_window = std::make_unique<AndroidDisplayWindow>(g_app_context);

            if (XFAILED(g_emulator->Setup(g_display_window.get(), &g_app_context, false, audio_factory, graphics_factory, input_factory))) {
                LOGE("Failed to setup emulator");
                g_emulator_running = false;
                return;
            }

            LOGI("Mounting standard drives");
            g_emulator->MountStandardDrives();

            LOGI("Launching game: %s", game_path.c_str());
            xe::X_STATUS result = g_emulator->LaunchPath(game_path);
            if (XFAILED(result)) {
                LOGE("Failed to launch game: %08X", result);
                g_emulator_running = false;
                return;
            }

            LOGI("Game running, waiting for exit...");
            g_emulator->WaitUntilExit();

            LOGI("Game exited successfully");

        } catch (const std::exception& e) {
            LOGE("Exception: %s", e.what());
        }
        g_emulator_running = false;
        g_display_window.reset();
    });

    env->ReleaseStringUTFChars(jgame_path, game_path);
}

JNIEXPORT void JNICALL
Java_jp_xenia_emulator_WindowDemoActivity_nativeShutdown(
    JNIEnv* env, jobject thiz) {

    LOGI("Shutting down emulator");
    g_emulator_running = false;

    if (g_emulator) {
        g_emulator->Shutdown();
        g_emulator.reset();
    }

    if (g_emulator_thread.joinable()) {
        g_emulator_thread.join();
    }
    
    g_display_window.reset();

    if (g_native_window) {
        ANativeWindow_release(g_native_window);
        g_native_window = nullptr;
    }

    LOGI("Shutdown complete");
}

JNIEXPORT jboolean JNICALL
Java_jp_xenia_emulator_WindowDemoActivity_nativeIsRunning(
    JNIEnv* env, jobject thiz) {
    return g_emulator_running ? JNI_TRUE : JNI_FALSE;
}

}
