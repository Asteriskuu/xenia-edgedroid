#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <thread>
#include <memory>
#include <string>
#include <filesystem>
#include <android/log.h>

#include "xenia/emulator.h"

#define LOG_TAG "XeniaAndroid"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static std::unique_ptr<xe::Emulator> g_emulator;
static std::thread g_emulator_thread;
static bool g_emulator_running = false;
static ANativeWindow* g_native_window = nullptr;

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

            LOGI("Setting up emulator...");
            if (XFAILED(g_emulator->Setup(nullptr, nullptr, false, nullptr, nullptr, nullptr))) {
                LOGE("Failed to setup emulator");
                g_emulator_running = false;
                return;
            }

            LOGI("Mounting standard drives...");
            g_emulator->MountStandardDrives();

            LOGI("Launching game path...");
            X_STATUS result = g_emulator->LaunchPath(game_path);
            if (XFAILED(result)) {
                LOGE("Failed to launch game: %08X", result);
                g_emulator_running = false;
                return;
            }

            LOGI("Game running, waiting for exit...");
            g_emulator->WaitUntilExit();

            LOGI("Game exited");

        } catch (const std::exception& e) {
            LOGE("Exception: %s", e.what());
        }
        g_emulator_running = false;
    });

    env->ReleaseStringUTFChars(jgame_path, game_path);
}

JNIEXPORT void JNICALL
Java_jp_xenia_emulator_WindowDemoActivity_nativeShutdown(
    JNIEnv* env, jobject thiz) {

    LOGI("Shutting down emulator...");
    g_emulator_running = false;

    if (g_emulator) {
        g_emulator->Shutdown();
        g_emulator.reset();
    }

    if (g_emulator_thread.joinable()) {
        g_emulator_thread.join();
    }

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
