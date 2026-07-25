#if defined(ANDROID)

#include <jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

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
std::atomic<bool> gImmersiveHostReady(false);
std::atomic<bool> gHandoffActive(false);

EGLDisplay gBootstrapDisplay = EGL_NO_DISPLAY;
EGLContext gBootstrapContext = EGL_NO_CONTEXT;
EGLSurface gBootstrapWindowSurface = EGL_NO_SURFACE;
EGLSurface gBootstrapParkingSurface = EGL_NO_SURFACE;

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

bool ParkExistingSdlContextBeforeHost() {
    gBootstrapDisplay = eglGetCurrentDisplay();
    gBootstrapContext = eglGetCurrentContext();
    gBootstrapWindowSurface = eglGetCurrentSurface(EGL_DRAW);
    if (gBootstrapDisplay == EGL_NO_DISPLAY ||
        gBootstrapContext == EGL_NO_CONTEXT ||
        gBootstrapWindowSurface == EGL_NO_SURFACE) {
        HandoffLog("cannot launch immersive host: current SDL EGL binding is incomplete");
        return false;
    }

    EGLint configId = 0;
    if (eglQueryContext(gBootstrapDisplay, gBootstrapContext,
                        EGL_CONFIG_ID, &configId) != EGL_TRUE) {
        HandoffLog("cannot launch immersive host: eglQueryContext(EGL_CONFIG_ID) failed");
        return false;
    }

    EGLConfig config = NULL;
    const EGLint configAttributes[] = {EGL_CONFIG_ID, configId, EGL_NONE};
    EGLint configCount = 0;
    if (eglChooseConfig(gBootstrapDisplay, configAttributes, &config, 1,
                        &configCount) != EGL_TRUE || configCount != 1) {
        HandoffLog("cannot launch immersive host: SDL EGLConfig could not be resolved");
        return false;
    }

    EGLint surfaceTypes = 0;
    if (eglGetConfigAttrib(gBootstrapDisplay, config, EGL_SURFACE_TYPE,
                           &surfaceTypes) != EGL_TRUE ||
        (surfaceTypes & EGL_PBUFFER_BIT) == 0) {
        HandoffLog("cannot launch immersive host: SDL EGLConfig has no pbuffer support");
        return false;
    }

    const EGLint pbufferAttributes[] = {
        EGL_WIDTH, 16,
        EGL_HEIGHT, 16,
        EGL_NONE
    };
    gBootstrapParkingSurface = eglCreatePbufferSurface(
            gBootstrapDisplay, config, pbufferAttributes);
    if (gBootstrapParkingSurface == EGL_NO_SURFACE) {
        HandoffLog("cannot launch immersive host: bootstrap pbuffer creation failed");
        return false;
    }

    glFinish();
    if (eglMakeCurrent(gBootstrapDisplay,
                       gBootstrapParkingSurface,
                       gBootstrapParkingSurface,
                       gBootstrapContext) != EGL_TRUE) {
        eglDestroySurface(gBootstrapDisplay, gBootstrapParkingSurface);
        gBootstrapParkingSurface = EGL_NO_SURFACE;
        HandoffLog("cannot launch immersive host: existing SDL context would not park");
        return false;
    }

    HandoffLog("existing SDL EGLContext parked before real IMMERSIVE_HMD host launch");
    return true;
}

void RestoreFlatContextAfterFailedHandoff() {
    if (gBootstrapDisplay != EGL_NO_DISPLAY &&
        gBootstrapContext != EGL_NO_CONTEXT &&
        gBootstrapWindowSurface != EGL_NO_SURFACE) {
        eglMakeCurrent(gBootstrapDisplay,
                       gBootstrapWindowSurface,
                       gBootstrapWindowSurface,
                       gBootstrapContext);
    }
    if (gBootstrapParkingSurface != EGL_NO_SURFACE &&
        gBootstrapDisplay != EGL_NO_DISPLAY &&
        eglGetCurrentSurface(EGL_DRAW) != gBootstrapParkingSurface) {
        eglDestroySurface(gBootstrapDisplay, gBootstrapParkingSurface);
        gBootstrapParkingSurface = EGL_NO_SURFACE;
    }
}

void ReleaseBootstrapParkingAfterOpenXR() {
    if (gBootstrapParkingSurface != EGL_NO_SURFACE &&
        gBootstrapDisplay != EGL_NO_DISPLAY &&
        eglGetCurrentSurface(EGL_DRAW) != gBootstrapParkingSurface) {
        eglDestroySurface(gBootstrapDisplay, gBootstrapParkingSurface);
        gBootstrapParkingSurface = EGL_NO_SURFACE;
        HandoffLog("bootstrap pbuffer released after OpenXR adopted the SDL context");
    }
}

bool RequestRealImmersiveHost() {
    if (javaVM == NULL || g_pJavaWrapper == NULL ||
        g_pJavaWrapper->activity == NULL) {
        HandoffLog("immersive host request failed: flat GameActivity/JavaVM unavailable");
        return false;
    }

    JNIEnv* env = CJavaWrapper::GetEnv();
    if (env == NULL) {
        HandoffLog("immersive host request failed: JNIEnv unavailable");
        return false;
    }

    jclass activityClass = env->GetObjectClass(g_pJavaWrapper->activity);
    if (activityClass == NULL) {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        HandoffLog("immersive host request failed: flat Activity class unavailable");
        return false;
    }

    jmethodID requestMethod = env->GetMethodID(
            activityClass, "requestImmersiveHandoff", "()V");
    if (requestMethod == NULL || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(activityClass);
        HandoffLog("immersive host request failed: Java launch method unavailable");
        return false;
    }

    gImmersiveHostReady.store(false, std::memory_order_release);
    HandoffLog("SDL context is parked; launching separate real IMMERSIVE_HMD host Activity");
    env->CallVoidMethod(g_pJavaWrapper->activity, requestMethod);
    env->DeleteLocalRef(activityClass);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        HandoffLog("immersive host request threw a Java exception");
        return false;
    }

    const timespec pause = {0, 2000000};
    for (int elapsedMs = 0; elapsedMs < 5000; elapsedMs += 2) {
        if (gImmersiveHostReady.load(std::memory_order_acquire)) {
            HandoffLog("real IMMERSIVE_HMD host registered; creating OpenXR against it");
            return true;
        }
        nanosleep(&pause, NULL);
    }

    HandoffLog("real IMMERSIVE_HMD host did not register within 5000 ms");
    return false;
}

} // namespace

// The proven QuestOpenXR::Initialize() implementation still owns loader,
// instance, session, swapchain and frame creation. This wrapper only ensures
// its Android Activity is a real foreground IMMERSIVE_HMD host, while its GLES
// binding remains the already-running SDL context parked above.
extern "C" bool __real__ZN11QuestOpenXR10InitializeEv();
extern "C" bool __wrap__ZN11QuestOpenXR10InitializeEv() {
    gHandoffActive.store(true, std::memory_order_release);

    if (!ParkExistingSdlContextBeforeHost()) {
        gHandoffActive.store(false, std::memory_order_release);
        return false;
    }
    if (!RequestRealImmersiveHost()) {
        RestoreFlatContextAfterFailedHandoff();
        gHandoffActive.store(false, std::memory_order_release);
        return false;
    }

    const bool initialized = __real__ZN11QuestOpenXR10InitializeEv();
    if (initialized) {
        ReleaseBootstrapParkingAfterOpenXR();
    } else {
        RestoreFlatContextAfterFailedHandoff();
    }
    gHandoffActive.store(false, std::memory_order_release);
    return initialized;
}

extern "C" JNIEXPORT void JNICALL
Java_com_revc_game_core_REVC_registerImmersiveHost(JNIEnv* env, jclass,
                                                   jobject activity) {
    if (env == NULL || activity == NULL) {
        HandoffLog("real IMMERSIVE_HMD host registration received null Activity");
        return;
    }
    if (javaVM == NULL) {
        env->GetJavaVM(&javaVM);
    }

    CJavaWrapper* replacement = new CJavaWrapper(env, activity);
    if (replacement == NULL || replacement->activity == NULL) {
        delete replacement;
        HandoffLog("real IMMERSIVE_HMD host global reference creation failed");
        return;
    }

    CJavaWrapper* previous = g_pJavaWrapper;
    g_pJavaWrapper = replacement;
    if (previous != NULL) {
        delete previous;
    }

    HandoffLog("native OpenXR Activity owner replaced with real IMMERSIVE_HMD host");
    gImmersiveHostReady.store(true, std::memory_order_release);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_revc_game_core_REVC_isOpenXrHandoffActive(JNIEnv*, jclass) {
    return gHandoffActive.load(std::memory_order_acquire)
            ? JNI_TRUE : JNI_FALSE;
}

#endif
