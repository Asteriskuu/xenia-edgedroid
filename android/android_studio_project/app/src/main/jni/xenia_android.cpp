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
#include <signal.h>
#include <setjmp.h>

#include "xenia/emulator.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/ui/vulkan/vulkan_provider.h"
#include "xenia/apu/nop/nop_audio_system.h"
#include "xenia/hid/nop/nop_input_driver.h"
#include "xenia/config.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/ui/surface.h"

#define LOG_TAG "XeniaAndroid"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

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

static void segfault_handler(int sig) {
    LOGE("[CRASH] Segmentation fault caught in emulator thread!");
    g_emulator_running = false;
}

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

    ANativeWindow_acquire(native_window);

    int width = 0, height = 0;
    const int max_wait_ms = 2000;
    int waited = 0;
    while (waited < max_wait_ms) {
      width = ANativeWindow_getWidth(native_window);
      height = ANativeWindow_getHeight(native_window);
      if (width > 0 && height > 0) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      waited += 50;
    }
    if (width <= 0 || height <= 0) {
      LOGI("nativeBootGame: surface still has 0 size after wait; continuing anyway");
    } else {
      LOGI("nativeBootGame: surface size detected %dx%d", width, height);
    }

    g_native_window = native_window;
    g_emulator_running = true;

    g_emulator_thread = std::thread([game_path = std::string(game_path), native_window]() {
        signal(SIGSEGV, segfault_handler);
        signal(SIGABRT, segfault_handler);
        
        try {
            LOGI("[Tracer] Thread started, game_path: %s", game_path.c_str());
            
            if (!std::filesystem::exists(game_path)) {
                LOGE("[ERROR] Game file does not exist: %s", game_path.c_str());
                g_emulator_running = false;
                return;
            }
            LOGI("[Tracer] File exists, size: %zu bytes", std::filesystem::file_size(game_path));
            
            std::filesystem::path storage_root = "/data/data/jp.xenia.emulator.github.debug";
            std::filesystem::create_directories(storage_root / "content");
            std::filesystem::create_directories(storage_root / "cache");
            LOGI("[Tracer] Storage directories created");

            LOGI("[Tracer] Creating emulator instance...");
            g_emulator = std::make_unique<xe::Emulator>(
                game_path, storage_root, storage_root / "content", storage_root / "cache"
            );
            LOGI("[Tracer] Emulator instance created successfully");

            LOGI("[Tracer] Loading config...");
            config::LoadGameConfigForFile(game_path);
            LOGI("[Tracer] Config loaded");

            auto graphics_factory = []() -> std::unique_ptr<xe::gpu::GraphicsSystem> {
                LOGI("[Tracer] Graphics factory invoked: probing Vulkan provider...");
                auto probe = xe::ui::vulkan::VulkanProvider::Create(false, false);
                if (!probe) {
                    LOGI("[Tracer] Vulkan provider probe failed - skipping Vulkan on this device");
                    return nullptr;
                }
                LOGI("[Tracer] Vulkan provider probe succeeded");
                return std::make_unique<xe::gpu::vulkan::VulkanGraphicsSystem>();
            };

            auto audio_factory = [](xe::cpu::Processor* processor) -> std::unique_ptr<xe::apu::AudioSystem> {
                LOGI("[Tracer] Audio factory invoked");
                return std::make_unique<xe::apu::nop::NopAudioSystem>(processor);
            };

            auto input_factory = [](xe::ui::Window* window) -> std::vector<std::unique_ptr<xe::hid::InputDriver>> {
                LOGI("[Tracer] Input factory invoked");
                std::vector<std::unique_ptr<xe::hid::InputDriver>> drivers;
                drivers.push_back(std::make_unique<xe::hid::nop::NopInputDriver>(window, 0));
                return drivers;
            };

            LOGI("[Tracer] Creating Display Window...");
            g_display_window = std::make_unique<AndroidDisplayWindow>(g_app_context);
            LOGI("[Tracer] Display window created");

            LOGI("[Tracer] Calling g_emulator->Setup()...");
            if (XFAILED(g_emulator->Setup(g_display_window.get(), nullptr, false, audio_factory, graphics_factory, input_factory))) {
                LOGE("[Tracer] SETUP FAILED!");
                g_emulator_running = false;
                return;
            }
            LOGI("[Tracer] Setup completed successfully");

            LOGI("[Tracer] Mounting standard drives...");
            g_emulator->MountStandardDrives();
            LOGI("[Tracer] Standard drives mounted");

            LOGI("[Tracer] About to call LaunchPath() with: %s", game_path.c_str());
            LOGI("[Tracer] Emulator state before launch: running=%d", g_emulator_running.load());
            
            if (!g_emulator) {
                LOGE("[ERROR] g_emulator is null before LaunchPath!");
                g_emulator_running = false;
                return;
            }

            xe::X_STATUS result = g_emulator->LaunchPath(game_path);
            
            LOGI("[Tracer] LaunchPath() returned: %08X", result);
            
            if (XFAILED(result)) {
                LOGE("[Tracer] LAUNCH FAILED with code: %08X", result);
                g_emulator_running = false;
                return;
            }

            LOGI("[Tracer] Game running! Entering standard WaitUntilExit() loop...");
            g_emulator->WaitUntilExit();
            LOGI("[Tracer] Game loop exited successfully");

        } catch (const std::exception& e) {
            LOGE("[EXCEPTION] Caught std::exception in emulator thread");
            LOGE("[EXCEPTION] Type: %s", typeid(e).name());
            LOGE("[EXCEPTION] Message: %s", e.what());
            g_emulator_running = false;
        } catch (...) {
            LOGE("[CRASH] Unknown fatal exception caught in emulator thread!");
            g_emulator_running = false;
        }
        
        LOGI("[Tracer] Cleaning up emulator thread...");
        g_emulator_running = false;
        g_display_window.reset();
        LOGI("[Tracer] Emulator thread cleanup complete");
    });

    env->ReleaseStringUTFChars(jgame_path, game_path);
}

JNIEXPORT void JNICALL
Java_jp_xenia_emulator_WindowDemoActivity_nativeShutdown(
    JNIEnv* env, jobject thiz) {

    LOGI("Shutting down emulator");
    g_emulator_running = false;

    if (g_emulator) {
        LOGI("Calling g_emulator->Shutdown()...");
        g_emulator->Shutdown();
        g_emulator.reset();
        LOGI("g_emulator shutdown complete");
    }

    if (g_emulator_thread.joinable()) {
        LOGI("Waiting for emulator thread to join...");
        g_emulator_thread.join();
        LOGI("Emulator thread joined");
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
