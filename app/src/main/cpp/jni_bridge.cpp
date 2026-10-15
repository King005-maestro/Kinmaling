#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <thread>
#include <atomic>
#include <memory>
#include "vk_context.h"
#include "box64_launcher.h"

static std::unique_ptr<VkContext> g_ctx;
static ANativeWindow* g_win = nullptr;
static std::atomic<bool> g_run{false};
static std::thread g_render, g_game;

static std::string str(JNIEnv* e, jstring s) {
    const char* c = e->GetStringUTFChars(s, nullptr);
    std::string r(c);
    e->ReleaseStringUTFChars(s, c);
    return r;
}

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_emu_mali_NativeBridge_onSurface(JNIEnv* env, jobject, jobject surface) {
    g_win = ANativeWindow_fromSurface(env, surface);
    g_ctx = std::make_unique<VkContext>();
    if (!g_ctx->init(g_win)) {
        __android_log_print(ANDROID_LOG_ERROR, "maliemu", "Vulkan init failed");
        return JNI_FALSE;
    }
    g_run = true;
    g_render = std::thread([] {
        float t = 0;
        while (g_run) { g_ctx->present(0.f, 0.f, 0.2f + 0.1f * (t += 0.01f > 1 ? -t : 0.01f)); }
    });
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_emu_mali_NativeBridge_onSurfaceDestroyed(JNIEnv*, jobject) {
    g_run = false;
    if (g_render.joinable()) g_render.join();
    if (g_ctx) { g_ctx->destroy(); g_ctx.reset(); }
    if (g_win) { ANativeWindow_release(g_win); g_win = nullptr; }
}

JNIEXPORT jboolean JNICALL
Java_com_emu_mali_NativeBridge_startFrameBridge(JNIEnv* env, jobject, jstring path) {
    return g_ctx && g_ctx->bridge().start(str(env, path));
}

JNIEXPORT void JNICALL
Java_com_emu_mali_NativeBridge_launch(JNIEnv* env, jobject, jstring box, jstring wine,
                                      jstring exe, jstring wd, jstring log) {
    std::string b = str(env, box), w = str(env, wine), x = str(env, exe), d = str(env, wd), l = str(env, log);
    if (g_game.joinable()) return;
    g_game = std::thread([=] { launchBox64(b, w, x, d, l); });
    g_game.detach();
}

JNIEXPORT void JNICALL
Java_com_emu_mali_NativeBridge_stop(JNIEnv*, jobject) { stopBox64(); }

JNIEXPORT jboolean JNICALL
Java_com_emu_mali_NativeBridge_hasFdm(JNIEnv*, jobject) {
    return g_ctx && g_ctx->hasFragmentDensityMap();
}

}
