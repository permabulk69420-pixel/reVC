//
// Created by mrxenginner on 14/07/2025.
//

#include "common.h"
#include "JavaWrapper.h"

#if defined ANDROID

extern "C" JavaVM *javaVM;

JNIEnv* CJavaWrapper::GetEnv() {
    if (!javaVM) {
        debug("GetEnv: JavaVM unavailable");
        return NULL;
    }

    JNIEnv* env = NULL;
    int getEnvStat = javaVM->GetEnv((void**)&env, JNI_VERSION_1_6);

    if (getEnvStat == JNI_EDETACHED) {
        debug("GetEnv: not attached");
        if (javaVM->AttachCurrentThread(&env, NULL) != 0) {
            debug("Failed to attach");
            return NULL;
        }
    }

    if (getEnvStat == JNI_EVERSION) {
        debug("GetEnv: version not supported");
        return NULL;
    }

    if (getEnvStat == JNI_ERR) {
        debug("GetEnv: JNI_ERR");
        return NULL;
    }
    return env;
}

void CJavaWrapper::ExitGame() {
    if (!this->activity || !this->s_ExitGame) {
        debug("ExitGame: Activity callback unavailable");
        return;
    }

    JNIEnv* env = GetEnv();
    if (!env) {
        debug("No env");
        return;
    }

    env->CallVoidMethod(this->activity, this->s_ExitGame);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

CJavaWrapper::CJavaWrapper(JNIEnv *env, jobject activity)
    : s_ExitGame(NULL), activity(NULL) {
    if (!env || !activity) {
        debug("CJavaWrapper: invalid Activity/JNIEnv");
        return;
    }

    this->activity = env->NewGlobalRef(activity);
    if (!this->activity) {
        debug("CJavaWrapper: NewGlobalRef failed");
        return;
    }

    jclass activityClass = env->GetObjectClass(activity);
    if (!activityClass) {
        debug("CJavaWrapper: Activity class unavailable");
        if (env->ExceptionCheck())
            env->ExceptionClear();
        return;
    }

    s_ExitGame = env->GetMethodID(activityClass, "exitGame", "()V");
    if (env->ExceptionCheck()) {
        // The Activity callback is optional for startup; never leave a pending
        // NoSuchMethodError that poisons the following OpenXR JNI calls.
        env->ExceptionClear();
        s_ExitGame = NULL;
    }
    env->DeleteLocalRef(activityClass);
}

CJavaWrapper::~CJavaWrapper() {
    if (!this->activity)
        return;

    JNIEnv* env = GetEnv();
    if (env)
        env->DeleteGlobalRef(this->activity);
    this->activity = NULL;
}

CJavaWrapper* g_pJavaWrapper = nullptr;
#endif
