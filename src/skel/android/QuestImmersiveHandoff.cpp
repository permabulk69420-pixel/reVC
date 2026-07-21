#if defined(ANDROID)

#include <jni.h>
#include <android/log.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <sys/stat.h>
#include <time.h>

#include "JavaWrapper.h"

extern JavaVM* javaVM;
extern CJavaWrapper* g_pJavaWrapper;

namespace {

const char* const kTag = "reVC-XR";
std::atomic<bool> gImmersiveAliasReady(false);

void HandoffLog(const char* message) {
    __android_log_write(ANDROID_LOG_INFO, kTag, message);

    const char* root = getenv("STORAGE_ROOT");
    if (root == NULL || root[0] == '\0') {
        return;
    }

    std::string gameRoot(root);
    while (!gameRoot.empty() && gameRoot[gameRoot.size() - 1] == '/') {
        gameRoot.erase(gameRoot.size() - 1);
    }
    const std::string userFiles = gameRoot + "/userfiles";
    if (mkdir(userFiles.c_str(), 0775) != 0 && errno != EEXIST) {
        return;
    }

    FILE* file = fopen((userFiles + "/xr_log.txt").c_str(), "a");
    if (file != NULL) {
        fprintf(file, "HANDOFF %s\n", message);
        fclose(file);
    }
}

bool RequestImmersiveAlias() {
    if (javaVM == NULL || g_pJavaWrapper == NULL ||
        g_pJavaWrapper->activity == NULL) {
        HandoffLog("immersive alias request failed: GameActivity/JavaVM unavailable");
        return false;
    }

    JNIEnv* env = CJavaWrapper::GetEnv();
    if (env == NULL) {
        HandoffLog("immersive alias request failed: JNIEnv unavailable");
        return false;
    }

    jclass activityClass = env->GetObjectClass(g_pJavaWrapper->activity);
    if (activityClass == NULL) {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        HandoffLog("immersive alias request failed: Activity class unavailable");
        return false;
    }

    jmethodID requestMethod = env->GetMethodID(
            activityClass, "requestImmersiveHandoff", "()V");
    if (requestMethod == NULL || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(activityClass);
        HandoffLog("immersive alias request failed: Java method unavailable");
        return false;
    }

    gImmersiveAliasReady.store(false);
    HandoffLog("OpenXR session exists; launching same GameActivity through IMMERSIVE_HMD alias");
    env->CallVoidMethod(g_pJavaWrapper->activity, requestMethod);
    env->DeleteLocalRef(activityClass);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        HandoffLog("immersive alias request threw a Java exception");
        return false;
    }

    const timespec pause = {0, 2000000};
    for (int elapsedMs = 0; elapsedMs < 4000; elapsedMs += 2) {
        if (gImmersiveAliasReady.load()) {
            HandoffLog("IMMERSIVE_HMD alias reached the existing single-task GameActivity");
            return true;
        }
        nanosleep(&pause, NULL);
    }

    HandoffLog("immersive alias did not return to GameActivity within 4000 ms");
    return false;
}

} // namespace

// QuestOpenXR::Initialize() is already proven to create the session, park the
// existing SDL EGL context and create eye swapchains. Wrap only its return so
// the same Activity is promoted through an immersive alias before frame submit.
extern "C" bool __real__ZN11QuestOpenXR10InitializeEv();
extern "C" bool __wrap__ZN11QuestOpenXR10InitializeEv() {
    if (!__real__ZN11QuestOpenXR10InitializeEv()) {
        return false;
    }
    return RequestImmersiveAlias();
}

extern "C" JNIEXPORT void JNICALL
Java_com_revc_game_core_REVC_notifyImmersiveAliasReady(JNIEnv*, jclass) {
    gImmersiveAliasReady.store(true);
}

#endif
