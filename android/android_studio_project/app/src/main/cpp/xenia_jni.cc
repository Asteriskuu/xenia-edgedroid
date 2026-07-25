#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <thread>
#include <memory>
#include <string>
#include <filesystem>

#include "xenia/emulator.h"

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
        env->ReleaseStringUTFChars(jgame_path, game_path);
        return;
    }

    ANativeWindow_acquire(native_window);
    g_native_window = native_window;
    g_emulator_running = true;

    g_emulator_thread = std::thread([game_path = std::string(game_path), native_window]() {
        try {
            std::filesystem::path storage_root = "/data/data/jp.xenia.emulator";
            std::filesystem::create_directories(storage_root / "content");
            std::filesystem::create_directories(storage_root / "cache");

            g_emulator = std::make_unique<xe::Emulator>(
                game_path,
                storage_root,
                storage_root / "content",
                storage_root / "cache"
            );

            if (XFAILED(g_emulator->Setup(nullptr, nullptr, false, nullptr, nullptr, nullptr))) {
                g_emulator_running = false;
                return;
            }

            g_emulator->MountStandardDrives();

            X_STATUS result = g_emulator->LaunchPath(game_path);
            if (XFAILED(result)) {
                g_emulator_running = false;
                return;
            }

            g_emulator->WaitUntilExit();

        } catch (const std::exception& e) {
        }
        g_emulator_running = false;
    });

    env->ReleaseStringUTFChars(jgame_path, game_path);
}

JNIEXPORT void JNICALL
Java_jp_xenia_emulator_WindowDemoActivity_nativeShutdown(
    JNIEnv* env, jobject thiz) {

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
}

JNIEXPORT jboolean JNICALL
Java_jp_xenia_emulator_WindowDemoActivity_nativeIsRunning(
    JNIEnv* env, jobject thiz) {
    return g_emulator_running ? JNI_TRUE : JNI_FALSE;
}

}
