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
#include <unistd.h>
#include <sys/prctl.h>
#include <pthread.h>

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
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

static std::unique_ptr<xe::Emulator> g_emulator;
static std::thread g_emulator_thread;
static std::atomic<bool> g_emulator_running{false};
static ANativeWindow* g_native_window = nullptr;
static std::mutex g_state_mutex;
static std::condition_variable g_state_condition;

static void flush_logs() {
    fflush(stdout);
    fflush(stderr);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[LOG FLUSH]");
}

static int pfd[2];
static void *log_redirect_thread_func(void *) {
    ssize_t rsize;
    char buf[128];
    while ((rsize = read(pfd[0], buf, sizeof buf - 1)) > 0) {
        if (buf[rsize - 1] == '\n') --rsize;
        buf[rsize] = 0;
        __android_log_write(ANDROID_LOG_INFO, LOG_TAG, buf);
    }
    return 0;
}

static void init_log_redirect() {
    if (pipe(pfd) == -1) {
        __android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "Failed to create pipe for log redirection");
        return;
    }
    
    int stdout_dup = dup2(pfd[1], STDOUT_FILENO);
    int stderr_dup = dup2(pfd[1], STDERR_FILENO);
    
    if (stdout_dup == -1 || stderr_dup == -1) {
        __android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "Failed to redirect stdout/stderr");
        return;
    }
    
    pthread_t tid;
    if (pthread_create(&tid, nullptr, log_redirect_thread_func, nullptr) != 0) {
        __android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "Failed to create log redirect thread");
        return;
    }
    pthread_detach(tid);
    
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);
}

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
            flush_logs();
            return false;
        }
        
        int attempts = 0;
        const int max_attempts = 30;
        
        do {
            width_out = static_cast<uint32_t>(ANativeWindow_getWidth(window_));
            height_out = static_cast<uint32_t>(ANativeWindow_getHeight(window_));
            
            if (width_out > 0 && height_out > 0) {
                LOGI("[Surface] Size query succeeded: %dx%d (attempt %d)", width_out, height_out, attempts + 1);
                flush_logs();
                return true;
            }
            
            if (attempts < max_attempts - 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            attempts++;
        } while (attempts < max_attempts && width_out == 0 && height_out == 0);

        if (width_out == 0 || height_out == 0) {
            LOGE("[Surface] CRITICAL: Window size is 0x0 after %d attempts (3 seconds total)! Vulkan swapchain creation will fail!", max_attempts);
            LOGE("[Surface] This usually means the layout hasn't been finalized yet.");
            LOGE("[Surface] Ensure the Activity waits before calling nativeBootGame()");
            flush_logs();
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
        flush_logs();
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
            LOGE("[Window] ANativeWindow is null! Surface cannot be created.");
            flush_logs();
            return nullptr;
        }
        LOGI("[Window] Creating Xenia Android Surface successfully.");
        flush_logs();
        return std::make_unique<xe::ui::AndroidSurface>(g_native_window);
    }
};
static std::unique_ptr<AndroidDisplayWindow> g_display_window;

static void segfault_handler(int sig) {
    LOGE("[CRASH] Segmentation fault caught in emulator thread! (signal %d)", sig);
    flush_logs();
    g_emulator_running = false;
    g_state_condition.notify_all();
}

static void abort_handler(int sig) {
    LOGE("[CRASH] Abort signal caught in emulator thread! (signal %d)", sig);
    flush_logs();
    g_emulator_running = false;
    g_state_condition.notify_all();
}

extern "C" {

JNIEXPORT void JNICALL
Java_jp_xenia_emulator_WindowDemoActivity_nativeBootGame(
    JNIEnv* env, jobject thiz,
    jstring jgame_path, jobject surface) {

    init_log_redirect();
    
    LOGI("[JNI] ========== nativeBootGame called ==========");
    flush_logs();
    
    const char* game_path = env->GetStringUTFChars(jgame_path, nullptr);
    if (!game_path) {
        LOGE("[JNI] Failed to get game path string");
        flush_logs();
        return;
    }

    ANativeWindow* native_window = ANativeWindow_fromSurface(env, surface);
    if (!native_window) {
        LOGE("[JNI] Failed to get native window from Surface");
        flush_logs();
        env->ReleaseStringUTFChars(jgame_path, game_path);
        return;
    }

    ANativeWindow_acquire(native_window);
    LOGI("[JNI] Native window acquired. Size: %dx%d", 
         ANativeWindow_getWidth(native_window), 
         ANativeWindow_getHeight(native_window));
    flush_logs();

    g_native_window = native_window;
    g_emulator_running = true;

    g_emulator_thread = std::thread([game_path_str = std::string(game_path), native_window]() {
        prctl(PR_SET_NAME, (unsigned long)"XeniaEmulator", 0, 0, 0);
        
        signal(SIGSEGV, segfault_handler);
        signal(SIGABRT, abort_handler);
        signal(SIGTERM, segfault_handler);
        
        LOGI("[Emulator] ========== EMULATOR THREAD STARTED ==========");
        LOGI("[Emulator] Game path: %s", game_path_str.c_str());
        flush_logs();
        
        try {
            LOGI("[Emulator] Step 1: Checking if game file exists...");
            flush_logs();
            if (!std::filesystem::exists(game_path_str)) {
                LOGE("[ERROR] Game file does not exist: %s", game_path_str.c_str());
                flush_logs();
                g_emulator_running = false;
                g_state_condition.notify_all();
                return;
            }
            LOGI("[Emulator] Step 1: File exists, size: %zu bytes", std::filesystem::file_size(game_path_str));
            flush_logs();
        
            std::filesystem::path storage_root = "/data/data/jp.xenia.emulator.github.debug";
            LOGI("[Emulator] Step 2: Creating storage directories at: %s", storage_root.c_str());
            flush_logs();
            try {
                std::filesystem::create_directories(storage_root / "content");
                std::filesystem::create_directories(storage_root / "cache");
                LOGI("[Emulator] Step 2: Storage directories created successfully");
                flush_logs();
            } catch (const std::exception& e) {
                LOGE("[ERROR] Failed to create storage directories: %s", e.what());
                flush_logs();
                g_emulator_running = false;
                g_state_condition.notify_all();
                return;
            }

            LOGI("[Emulator] Step 3: Creating emulator instance...");
            flush_logs();
            try {
                g_emulator = std::make_unique<xe::Emulator>(
                    game_path_str, storage_root, storage_root / "content", storage_root / "cache"
                );
                LOGI("[Emulator] Step 3: Emulator instance created successfully");
                flush_logs();
            } catch (const std::exception& e) {
                LOGE("[ERROR] Step 3 FAILED: Failed to create emulator: %s", e.what());
                LOGE("[ERROR] Exception type: %s", typeid(e).name());
                flush_logs();
                g_emulator_running = false;
                g_state_condition.notify_all();
                return;
            } catch (...) {
                LOGE("[ERROR] Step 3 FAILED: Unknown exception creating emulator");
                flush_logs();
                g_emulator_running = false;
                g_state_condition.notify_all();
                return;
            }

            LOGI("[Emulator] Step 4: Loading game config...");
            flush_logs();
            try {
                config::LoadGameConfigForFile(game_path_str);
                LOGI("[Emulator] Step 4: Config loaded successfully");
                flush_logs();
            } catch (const std::exception& e) {
                LOGE("[WARNING] Step 4: Failed to load config (non-fatal): %s", e.what());
                flush_logs();
            } catch (...) {
                LOGE("[WARNING] Step 4: Failed to load config (non-fatal, unknown error)");
                flush_logs();
            }

            LOGI("[Emulator] Step 5: Setting up graphics factory...");
            flush_logs();
            auto graphics_factory = []() -> std::unique_ptr<xe::gpu::GraphicsSystem> {
                LOGI("[Emulator]   Graphics factory invoked: creating VulkanGraphicsSystem...");
                flush_logs();
                try {
                    auto graphics_sys = std::make_unique<xe::gpu::vulkan::VulkanGraphicsSystem>();
                    LOGI("[Emulator]   VulkanGraphicsSystem created successfully");
                    flush_logs();
                    return graphics_sys;
                } catch (const std::exception& e) {
                    LOGE("[ERROR]   Failed to create VulkanGraphicsSystem: %s", e.what());
                    flush_logs();
                    return nullptr;
                } catch (...) {
                    LOGE("[ERROR]   Failed to create VulkanGraphicsSystem: unknown exception");
                    flush_logs();
                    return nullptr;
                }
            };

            LOGI("[Emulator] Step 6: Setting up audio factory...");
            flush_logs();
            auto audio_factory = [](xe::cpu::Processor* processor) -> std::unique_ptr<xe::apu::AudioSystem> {
                LOGI("[Emulator]   Audio factory invoked");
                flush_logs();
                if (!processor) {
                    LOGE("[ERROR]   Processor is null in audio factory!");
                    flush_logs();
                    return nullptr;
                }
                try {
                    auto audio_sys = std::make_unique<xe::apu::nop::NopAudioSystem>(processor);
                    LOGI("[Emulator]   NopAudioSystem created successfully");
                    flush_logs();
                    return audio_sys;
                } catch (const std::exception& e) {
                    LOGE("[ERROR]   Failed to create NopAudioSystem: %s", e.what());
                    flush_logs();
                    return nullptr;
                }
            };

            LOGI("[Emulator] Step 7: Setting up input factory...");
            flush_logs();
            auto input_factory = [](xe::ui::Window* window) -> std::vector<std::unique_ptr<xe::hid::InputDriver>> {
                LOGI("[Emulator]   Input factory invoked");
                flush_logs();
                
                if (!window) {
                    LOGW("[Emulator]   Window is null in input factory, returning empty driver list");
                    flush_logs();
                    return {};
                }
                
                std::vector<std::unique_ptr<xe::hid::InputDriver>> drivers;
                
                try {
                    LOGI("[Emulator]   Creating NopInputDriver...");
                    flush_logs();
                    auto driver = std::make_unique<xe::hid::nop::NopInputDriver>(window, 0);
                    if (driver) {
                        drivers.push_back(std::move(driver));
                        LOGI("[Emulator]   NopInputDriver created successfully");
                        flush_logs();
                    }
                } catch (const std::exception& e) {
                    LOGE("[ERROR]   Failed to create NopInputDriver: %s", e.what());
                    LOGE("[ERROR]   Exception type: %s", typeid(e).name());
                    flush_logs();
                } catch (...) {
                    LOGE("[ERROR]   Failed to create NopInputDriver: unknown exception");
                    flush_logs();
                }
                
                LOGI("[Emulator]   Input factory returning %zu drivers", drivers.size());
                flush_logs();
                return drivers;
            };

            LOGI("[Emulator] Step ?: Creating display window...");
            flush_logs();
            try {
                g_display_window = std::make_unique<AndroidDisplayWindow>(g_app_context);
                LOGI("[Emulator] Step ?: Display window created successfully");
                flush_logs();
            } catch (const std::exception& e) {
                LOGE("[ERROR] Step ? FAILED: Failed to create display window: %s", e.what());
                LOGE("[ERROR] Exception type: %s", typeid(e).name());
                flush_logs();
                g_emulator_running = false;
                g_state_condition.notify_all();
                return;
            } catch (...) {
                LOGE("[ERROR] Step ? FAILED: Unknown exception creating display window");
                flush_logs();
                g_emulator_running = false;
                g_state_condition.notify_all();
                return;
            }

            LOGI("[Emulator] Step 0: Mounting standard drives...");
            flush_logs();
            try {
                if (!g_emulator) {
                    LOGE("[ERROR] Step 0: g_emulator is null!");
                    flush_logs();
                    g_emulator_running = false;
                    g_state_condition.notify_all();
                    return;
                }
                g_emulator->MountStandardDrives();
                LOGI("[Emulator] Step 0: Standard drives mounted successfully");
                flush_logs();
            } catch (const std::exception& e) {
                LOGE("[ERROR] Step 0 FAILED: Failed to mount drives: %s", e.what());
                flush_logs();
                g_emulator_running = false;
                g_state_condition.notify_all();
                return;
            } catch (...) {
                LOGE("[ERROR] Step 0 FAILED: Unknown exception mounting drives");
                flush_logs();
                g_emulator_running = false;
                g_state_condition.notify_all();
                return;
            }

            LOGI("[Emulator] Step 10: Calling g_emulator->Setup()...");
            LOGD("[Emulator]   g_display_window: %p", g_display_window.get());
            LOGD("[Emulator]   g_emulator: %p", g_emulator.get());
            flush_logs();
            
            xe::X_STATUS setup_result = static_cast<xe::X_STATUS>(0xC0000001L);
            try {
                setup_result = g_emulator->Setup(
                    g_display_window.get(), nullptr, false, 
                    audio_factory, graphics_factory, input_factory
                );
                LOGI("[Emulator] Step 10: Setup returned with code: 0x%08X", setup_result);
                flush_logs();
            } catch (const std::exception& e) {
                LOGE("[ERROR] Step 10 FAILED: Exception during Setup: %s", e.what());
                LOGE("[ERROR] Exception type: %s", typeid(e).name());
                flush_logs();
                g_emulator_running = false;
                g_display_window.reset();
                g_emulator.reset();
                g_state_condition.notify_all();
                return;
            } catch (...) {
                LOGE("[ERROR] Step 10 FAILED: Unknown exception during Setup");
                flush_logs();
                g_emulator_running = false;
                g_display_window.reset();
                g_emulator.reset();
                g_state_condition.notify_all();
                return;
            }
            
            if (XFAILED(setup_result)) {
                LOGE("[ERROR] Step 10: Setup failed with code: 0x%08X", setup_result);
                flush_logs();
                g_emulator_running = false;
                g_display_window.reset();
                g_emulator.reset();
                g_state_condition.notify_all();
                return;
            }
            LOGI("[Emulator] Step 10: Setup completed successfully");
            flush_logs();

            LOGI("[Emulator] Step 11: Launching game with LaunchPath()...");
            LOGI("[Emulator]   Path: %s", game_path_str.c_str());
            flush_logs();
            
            if (!g_emulator) {
                LOGE("[ERROR] Step 11: g_emulator is null before LaunchPath!");
                flush_logs();
                g_emulator_running = false;
                g_state_condition.notify_all();
                return;
            }

            xe::X_STATUS launch_result = static_cast<xe::X_STATUS>(0xC0000001L);
            try {
                launch_result = g_emulator->LaunchPath(game_path_str);
                LOGI("[Emulator] Step 11: LaunchPath() returned: 0x%08X", launch_result);
                flush_logs();
            } catch (const std::exception& e) {
                LOGE("[ERROR] Step 11 FAILED: Exception during LaunchPath: %s", e.what());
                LOGE("[ERROR] Exception type: %s", typeid(e).name());
                flush_logs();
                g_emulator_running = false;
                g_state_condition.notify_all();
                return;
            } catch (...) {
                LOGE("[ERROR] Step 11 FAILED: Unknown exception during LaunchPath");
                flush_logs();
                g_emulator_running = false;
                g_state_condition.notify_all();
                return;
            }
            
            if (XFAILED(launch_result)) {
                LOGE("[ERROR] Step 11: Launch failed with code: 0x%08X", launch_result);
                flush_logs();
                g_emulator_running = false;
                g_state_condition.notify_all();
                return;
            }

            LOGI("[Emulator] Step 12: Game launched! Entering WaitUntilExit() loop...");
            flush_logs();
            try {
                auto last_heartbeat = std::chrono::steady_clock::now();
                while (g_emulator_running) {
                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat).count() >= 2) {
                        LOGI("[Heartbeat] Emulator still running...");
                        flush_logs();
                        last_heartbeat = now;
                    }
                    
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                LOGI("[Emulator] Step 12: Game loop exited");
                flush_logs();
            } catch (const std::exception& e) {
                LOGE("[ERROR] Step 12 FAILED: Exception in WaitUntilExit: %s", e.what());
                flush_logs();
            } catch (...) {
                LOGE("[ERROR] Step 12 FAILED: Unknown exception in WaitUntilExit");
                flush_logs();
            }

        } catch (const std::exception& e) {
            LOGE("[EXCEPTION] Caught std::exception in emulator thread");
            LOGE("[EXCEPTION] Type: %s", typeid(e).name());
            LOGE("[EXCEPTION] Message: %s", e.what());
            flush_logs();
            g_emulator_running = false;
            g_state_condition.notify_all();
        } catch (...) {
            LOGE("[CRASH] Unknown fatal exception caught in emulator thread!");
            flush_logs();
            g_emulator_running = false;
            g_state_condition.notify_all();
        }
        
        LOGI("[Emulator] Cleaning up emulator thread...");
        flush_logs();
        try {
            if (g_display_window) {
                LOGI("[Emulator]   Resetting display window...");
                flush_logs();
                g_display_window.reset();
                LOGI("[Emulator]   Display window reset complete");
                flush_logs();
            }
            if (g_emulator) {
                LOGI("[Emulator]   Resetting emulator...");
                flush_logs();
                g_emulator.reset();
                LOGI("[Emulator]   Emulator reset complete");
                flush_logs();
            }
        } catch (const std::exception& e) {
            LOGE("[ERROR] Exception during cleanup: %s", e.what());
            flush_logs();
        } catch (...) {
            LOGE("[ERROR] Unknown exception during cleanup");
            flush_logs();
        }
        
        g_emulator_running = false;
        g_state_condition.notify_all();
        LOGI("[Emulator] ========== EMULATOR THREAD ENDED ==========");
        flush_logs();
    });

    env->ReleaseStringUTFChars(jgame_path, game_path);
    LOGI("[JNI] nativeBootGame returning, emulator thread launched");
    flush_logs();
}

JNIEXPORT void JNICALL
Java_jp_xenia_emulator_WindowDemoActivity_nativeShutdown(
    JNIEnv* env, jobject thiz) {

    LOGI("[JNI] ========== nativeShutdown called ==========");
    flush_logs();
    g_emulator_running = false;
    g_state_condition.notify_all();
    
    if (g_emulator) {
        LOGI("[JNI] Calling g_emulator->Shutdown()...");
        flush_logs();
        try {
            g_emulator->Shutdown();
            LOGI("[JNI] g_emulator->Shutdown() completed");
            flush_logs();
            g_emulator.reset();
            LOGI("[JNI] g_emulator reset complete");
            flush_logs();
        } catch (const std::exception& e) {
            LOGE("[ERROR] Exception during emulator shutdown: %s", e.what());
            flush_logs();
            g_emulator.reset();
        } catch (...) {
            LOGE("[ERROR] Unknown exception during emulator shutdown");
            flush_logs();
            g_emulator.reset();
        }
    }

    if (g_emulator_thread.joinable()) {
        LOGI("[JNI] Waiting for emulator thread to join (max 10 seconds)...");
        flush_logs();
        
        bool joined = false;
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
            if (!g_emulator_running) {
                LOGI("[JNI] Emulator thread has stopped, joining...");
                flush_logs();
                g_emulator_thread.join();
                joined = true;
                LOGI("[JNI] Emulator thread joined successfully");
                flush_logs();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (!joined) {
            LOGE("[ERROR] Emulator thread did not join within 10 second timeout!");
            flush_logs();
        }
    }
    
    if (g_display_window) {
        LOGI("[JNI] Resetting display window...");
        flush_logs();
        g_display_window.reset();
        LOGI("[JNI] Display window reset complete");
        flush_logs();
    }
    
    if (g_native_window) {
        LOGI("[JNI] Releasing native window...");
        flush_logs();
        ANativeWindow_release(g_native_window);
        g_native_window = nullptr;
        LOGI("[JNI] Native window released");
        flush_logs();
    }

    LOGI("[JNI] ========== SHUTDOWN COMPLETE ==========");
    flush_logs();
}

JNIEXPORT jboolean JNICALL
Java_jp_xenia_emulator_WindowDemoActivity_nativeIsRunning(
    JNIEnv* env, jobject thiz) {
    return g_emulator_running ? JNI_TRUE : JNI_FALSE;
}

}  // extern "C"
