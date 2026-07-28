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
#include <mutex>
#include <condition_variable>

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
static std::mutex g_state_mutex;
static std::condition_variable g_state_condition;

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
        
        int attempts = 0;
        const int max_attempts = 10;
        
        do {
            width_out = static_cast<uint32_t>(ANativeWindow_getWidth(window_));
            height_out = static_cast<uint32_t>(ANativeWindow_getHeight(window_));
            
            if (width_out > 0 && height_out > 0) {
                LOGI("[Surface] Size query succeeded: %dx%d (attempt %d)", width_out, height_out, attempts + 1);
                return true;
            }
            
            if (attempts < max_attempts - 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            attempts++;
        } while (attempts < max_attempts && width_out == 0 && height_out == 0);

        if (width_out == 0 || height_out == 0) {
            LOGE("[Surface] CRITICAL: Window size is 0x0 after %d attempts! Vulkan swapchain creation will fail!", max_attempts);
            LOGE("[Surface] This usually means the layout hasn't been finalized yet.");
            LOGE("[Surface] Ensure the Activity waits for onLayoutChange() before calling nativeBootGame()");
            return false;
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
    void NotifyUILoopOfPendingFunctions() override {
    }
    
    void PlatformQuitFromUIThread() override {
        LOGI("[AppContext] Quit requested from UI thread");
        g_emulator_running = false;
        g_state_condition.notify_all();
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
    g_state_condition.notify_all();
}

static void abort_handler(int sig) {
    LOGE("[CRASH] Abort signal caught in emulator thread!");
    g_emulator_running = false;
    g_state_condition.notify_all();
}

extern "C" {

JNIEXPORT void JNICALL
Java_jp_xenia_emulator_WindowDemoActivity_nativeBootGame(
    JNIEnv* env, jobject thiz,
    jstring jgame_path, jobject surface) {

    LOGI("[JNI] nativeBootGame called");
    
    const char* game_path = env->GetStringUTFChars(jgame_path, nullptr);
    if (!game_path) {
        LOGE("[JNI] Failed to get game path string");
        return;
    }

    ANativeWindow* native_window = ANativeWindow_fromSurface(env, surface);
    if (!native_window) {
        LOGE("[JNI] Failed to get native window from Surface");
        env->ReleaseStringUTFChars(jgame_path, game_path);
        return;
    }

    ANativeWindow_acquire(native_window);
    LOGI("[JNI] Native window acquired. Size: %dx%d", 
         ANativeWindow_getWidth(native_window), 
         ANativeWindow_getHeight(native_window));

    g_native_window = native_window;
    g_emulator_running = true;

    g_emulator_thread = std::thread([game_path = std::string(game_path), native_window]() {
        signal(SIGSEGV, segfault_handler);
        signal(SIGABRT, abort_handler);
        signal(SIGTERM, segfault_handler);
        
        LOGI("[Emulator] Thread started, game_path: %s", game_path.c_str());
        
        try {
            if (!std::filesystem::exists(game_path)) {
                LOGE("[ERROR] Game file does not exist: %s", game_path.c_str());
                g_emulator_running = false;
                g_state_condition.notify_all();
                return;
            }
            LOGI("[Emulator] File exists, size: %zu bytes", std::filesystem::file_size(game_path));
        
            std::filesystem::path storage_root = "/data/data/jp.xenia.emulator.github.debug";
            try {
                std::filesystem::create_directories(storage_root / "content");
                std::filesystem::create_directories(storage_root / "cache");
                LOGI("[Emulator] Storage directories created");
            } catch (const std::exception& e) {
                LOGE("[ERROR] Failed to create storage directories: %s", e.what());
                g_emulator_running = false;
                g_state_condition.notify_all();
                return;
            }

            LOGI("[Emulator] Creating emulator instance...");
            try {
                g_emulator = std::make_unique<xe::Emulator>(
                    game_path, storage_root, storage_root / "content", storage_root / "cache"
                );
            } catch (const std::exception& e) {
                LOGE("[ERROR] Failed to create emulator: %s", e.what());
                g_emulator_running = false;
                g_state_condition.notify_all();
                return;
            }
            LOGI("[Emulator] Emulator instance created successfully");

            LOGI("[Emulator] Loading config...");
            try {
                config::LoadGameConfigForFile(game_path);
                LOGI("[Emulator] Config loaded");
            } catch (const std::exception& e) {
                LOGE("[ERROR] Failed to load config: %s", e.what());
            }

            auto graphics_factory = []() -> std::unique_ptr<xe::gpu::GraphicsSystem> {
                LOGI("[Emulator] Graphics factory invoked: probing Vulkan provider...");
                auto probe = xe::ui::vulkan::VulkanProvider::Create(false, false);
                if (!probe) {
                    LOGE("[ERROR] Vulkan provider probe failed - Vulkan not available on this device");
                    return nullptr;
                }
                LOGI("[Emulator] Vulkan provider probe succeeded");
                return std::make_unique<xe::gpu::vulkan::VulkanGraphicsSystem>();
            };

            auto audio_factory = [](xe::cpu::Processor* processor) -> std::unique_ptr<xe::apu::AudioSystem> {
                LOGI("[Emulator] Audio factory invoked");
                return std::make_unique<xe::apu::nop::NopAudioSystem>(processor);
            };

            auto input_factory = [](xe::ui::Window* window) -> std::vector<std::unique_ptr<xe::hid::InputDriver>> {
                LOGI("[Emulator] Input factory invoked");
                std::vector<std::unique_ptr<xe::hid::InputDriver>> drivers;
                drivers.push_back(std::make_unique<xe::hid::nop::NopInputDriver>(window, 0));
                return drivers;
            };

            LOGI("[Emulator] Creating Display Window...");
            try {
                g_display_window = std::make_unique<AndroidDisplayWindow>(g_app_context);
                LOGI("[Emulator] Display window created");
            } catch (const std::exception& e) {
                LOGE("[ERROR] Failed to create display window: %s", e.what());
                g_emulator_running = false;
                g_state_condition.notify_all();
                return;
            }

            LOGI("[Emulator] Calling g_emulator->Setup()...");
            xe::X_STATUS setup_result = g_emulator->Setup(
                g_display_window.get(), nullptr, false, 
                audio_factory, graphics_factory, input_factory
            );
            
            if (XFAILED(setup_result)) {
                LOGE("[ERROR] Setup failed with code: 0x%08X", setup_result);
                g_emulator_running = false;
                g_display_window.reset();
                g_emulator.reset();
                g_state_condition.notify_all();
                return;
            }
            LOGI("[Emulator] Setup completed successfully");

            LOGI("[Emulator] Mounting standard drives...");
            try {
                g_emulator->MountStandardDrives();
                LOGI("[Emulator] Standard drives mounted");
            } catch (const std::exception& e) {
                LOGE("[ERROR] Failed to mount drives: %s", e.what());
            }

            LOGI("[Emulator] About to call LaunchPath() with: %s", game_path.c_str());
            
            if (!g_emulator) {
                LOGE("[ERROR] g_emulator is null before LaunchPath!");
                g_emulator_running = false;
                g_state_condition.notify_all();
                return;
            }

            xe::X_STATUS launch_result = g_emulator->LaunchPath(game_path);
            LOGI("[Emulator] LaunchPath() returned: 0x%08X", launch_result);
            
            if (XFAILED(launch_result)) {
                LOGE("[ERROR] Launch failed with code: 0x%08X", launch_result);
                g_emulator_running = false;
                g_state_condition.notify_all();
                return;
            }

            LOGI("[Emulator] Game launching! Entering WaitUntilExit() loop...");
            g_emulator->WaitUntilExit();
            LOGI("[Emulator] Game loop exited successfully");

        } catch (const std::exception& e) {
            LOGE("[EXCEPTION] Caught std::exception in emulator thread");
            LOGE("[EXCEPTION] Type: %s", typeid(e).name());
            LOGE("[EXCEPTION] Message: %s", e.what());
            g_emulator_running = false;
            g_state_condition.notify_all();
        } catch (...) {
            LOGE("[CRASH] Unknown fatal exception caught in emulator thread!");
            g_emulator_running = false;
            g_state_condition.notify_all();
        }
        
        LOGI("[Emulator] Cleaning up emulator thread...");
        try {
            if (g_display_window) {
                g_display_window.reset();
            }
            if (g_emulator) {
                g_emulator.reset();
            }
        } catch (const std::exception& e) {
            LOGE("[ERROR] Exception during cleanup: %s", e.what());
        }
        
        g_emulator_running = false;
        g_state_condition.notify_all();
        LOGI("[Emulator] Emulator thread cleanup complete");
    });

    env->ReleaseStringUTFChars(jgame_path, game_path);
    LOGI("[JNI] nativeBootGame returning, emulator thread launched");
}

JNIEXPORT void JNICALL
Java_jp_xenia_emulator_WindowDemoActivity_nativeShutdown(
    JNIEnv* env, jobject thiz) {

    LOGI("[JNI] nativeShutdown called");
    g_emulator_running = false;
    g_state_condition.notify_all();
    
    if (g_emulator) {
        LOGI("[JNI] Calling g_emulator->Shutdown()...");
        try {
            g_emulator->Shutdown();
            g_emulator.reset();
            LOGI("[JNI] g_emulator shutdown complete");
        } catch (const std::exception& e) {
            LOGE("[ERROR] Exception during emulator shutdown: %s", e.what());
            g_emulator.reset();
        }
    }

    if (g_emulator_thread.joinable()) {
        LOGI("[JNI] Waiting for emulator thread to join (max 10 seconds)...");
        
        bool joined = false;
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
            if (!g_emulator_running) {
                g_emulator_thread.join();
                joined = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (!joined) {
            LOGE("[ERROR] Emulator thread did not join within timeout!");
        } else {
            LOGI("[JNI] Emulator thread joined");
        }
    }
    
    if (g_display_window) {
        g_display_window.reset();
    }
    
    if (g_native_window) {
        ANativeWindow_release(g_native_window);
        g_native_window = nullptr;
        LOGI("[JNI] Native window released");
    }

    LOGI("[JNI] Shutdown complete");
}

JNIEXPORT jboolean JNICALL
Java_jp_xenia_emulator_WindowDemoActivity_nativeIsRunning(
    JNIEnv* env, jobject thiz) {
    return g_emulator_running ? JNI_TRUE : JNI_FALSE;
}

}  // extern "C"
