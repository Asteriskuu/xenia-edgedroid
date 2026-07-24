#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <thread>
#include <memory>
#include <string>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <android/log.h>

#include "xenia/emulator.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/apu/nop/nop_audio_system.h"
#include "xenia/hid/nop/nop_input_driver.h"
#include "xenia/config.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/ui/surface.h"

#define LOG_TAG "XeniaAndroid"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static std::unique_ptr<xe::Emulator> g_emulator;
static std::thread g_emulator_thread;
static std::atomic<bool> g_emulator_running{false};
static ANativeWindow* g_native_window = nullptr;

namespace xe {
namespace ui {
class AndroidSurface : public Surface {
public:
    AndroidSurface(ANativeWindow* window) : window_(window) {}
    ~AndroidSurface() override = default;

    TypeIndex GetType() const override {
        return kTypeIndex_AndroidNativeWindow;
    }

    bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override {
        if (!window_) {
            LOGE("[Surface] Native window is null!");
            return false;
        }
        
        width_out = static_cast<uint32_t>(ANativeWindow_getWidth(window_));
        height_out = static_cast<uint32_t>(ANativeWindow_getHeight(window_));
        
        LOGI("[Surface] Size requested by Vulkan: %dx%d", width_out, height_out);
        
        if (width_out == 0 || height_out == 0) {
            LOGE("[Surface] DANGER: Window size is 0x0. Vulkan swapchain creation will likely fail/hang!");
        }
        return true;
    }

    ANativeWindow* native_window() const { return window_; }

private:
    ANativeWindow* window_;
};
}  // namespace ui
}  // namespace xe

class AndroidAppContext : public xe::ui::WindowedAppContext {
public:
    void NotifyUILoopOfPendingFunctions() override {}
    void PlatformQuitFromUIThread() override {
        LOGI("[AppContext] Quit requested from UI thread");
        g_emulator_running = false;
    }
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

    bool OpenImpl() override { return true; }
    void RequestCloseImpl() override {}
    void RequestPaintImpl() override {}

    std::unique_ptr<xe::ui::Surface> CreateSurfaceImpl(xe::ui::Surface::TypeFlags allowed_types) override {
        if (!g_native_window) {
            LOGE("ANativeWindow is null! Surface cannot be created.");
            return nullptr;
        }
        LOGI("Creating Xenia Android Surface successfully.");
        return std::make_unique<xe::ui::AndroidSurface>(g_native_window);
    }
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

    int width = ANativeWindow_getWidth(native_window);
    int height = ANativeWindow_getHeight(native_window);
    LOGI("Booting game: %s | Surface Init Size: %dx%d", game_path, width, height);

    g_native_window = native_window;
    g_emulator_running = true;

    g_emulator_thread = std::thread([game_path = std::string(game_path), native_window]() {
        try {
            std::filesystem::path storage_root = "/data/data/jp.xenia.emulator";
            std::filesystem::create_directories(storage_root / "content");
            std::filesystem::create_directories(storage_root / "cache");

            LOGI("[Tracer] Creating emulator instance...");
            g_emulator = std::make_unique<xe::Emulator>(
                game_path, storage_root, storage_root / "content", storage_root / "cache"
            );

            LOGI("[Tracer] Loading config...");
            config::LoadGameConfigForFile(game_path);

            auto graphics_factory = []() -> std::unique_ptr<xe::gpu::GraphicsSystem> {
                LOGI("[Tracer] Graphics factory invoked");
                return std::make_unique<xe::gpu::vulkan::VulkanGraphicsSystem>();
            };

            auto audio_factory = [](xe::cpu::Processor* processor) -> std::unique_ptr<xe::apu::AudioSystem> {
                return std::make_unique<xe::apu::nop::NopAudioSystem>(processor);
            };

            auto input_factory = [](xe::ui::Window* window) -> std::vector<std::unique_ptr<xe::hid::InputDriver>> {
                std::vector<std::unique_ptr<xe::hid::InputDriver>> drivers;
                drivers.push_back(std::make_unique<xe::hid::nop::NopInputDriver>(window, 0));
                return drivers;
            };

            LOGI("[Tracer] Creating Display Window...");
            g_display_window = std::make_unique<AndroidDisplayWindow>(g_app_context);

            LOGI("[Tracer] Calling g_emulator->Setup(). This might block or crash!");
            if (XFAILED(g_emulator->Setup(g_display_window.get(), nullptr, false, audio_factory, graphics_factory, input_factory))) {
                LOGE("[Tracer] SETUP FAILED!");
                g_emulator_running = false;
                return;
            }

            LOGI("[Tracer] Setup complete. Mounting standard drives...");
            g_emulator->MountStandardDrives();

            LOGI("[Tracer] Calling LaunchPath()...");
            xe::X_STATUS result = g_emulator->LaunchPath(game_path);
            if (XFAILED(result)) {
                LOGE("[Tracer] LAUNCH FAILED with code: %08X", result);
                g_emulator_running = false;
                return;
            }

            LOGI("[Tracer] Game running! Entering manual UI pump loop...");
            
            while (g_emulator_running) {
                g_app_context.ExecutePendingFunctions();
                
                if (g_emulator->has_requested_exit()) {
                    LOGI("[Tracer] Emulator requested exit internally.");
                    break;
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }

            LOGI("Game loop exited successfully");

        } catch (const std::exception& e) {
            LOGE("Exception caught in emulator thread: %s", e.what());
        } catch (...) {
            LOGE("Unknown fatal exception caught in emulator thread!");
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
