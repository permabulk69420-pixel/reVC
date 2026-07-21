#if defined(ANDROID)

#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#ifndef XR_USE_PLATFORM_ANDROID
#define XR_USE_PLATFORM_ANDROID
#endif
#ifndef XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_GRAPHICS_API_OPENGL_ES
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "QuestOpenXR.h"
#include "JavaWrapper.h"

#include <android/log.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

extern JavaVM* javaVM;

namespace {

const char* const kLogTag = "reVC-XR";
const XrViewConfigurationType kViewType =
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

struct Swapchain {
    XrSwapchain handle;
    int32_t width;
    int32_t height;
    std::vector<XrSwapchainImageOpenGLESKHR> images;

    Swapchain() : handle(XR_NULL_HANDLE), width(0), height(0) {}
};

struct SavedGlState {
    GLint drawFramebuffer;
    GLint readFramebuffer;
    GLint viewport[4];
    GLint scissor[4];
    GLfloat clearColor[4];
    GLboolean colorMask[4];
    GLboolean scissorEnabled;
};

struct State {
    bool attempted;
    bool initialized;
    bool sessionRunning;
    bool fatalError;
    bool parkingActive;
    bool parkingSurfaceless;
    bool headOriginSet;
    bool headValid;
    bool loggedWaitFrame;
    bool loggedBeginFrame;
    bool loggedEndFrame;
    std::atomic<bool> ownsPresentation;
    std::atomic<bool> exitRequested;
    pid_t ownerThread;
    uint64_t frameCount;

    FILE* logFile;
    std::string gameRoot;

    XrInstance instance;
    XrSystemId systemId;
    XrSession session;
    XrSpace appSpace;
    XrSessionState sessionState;

    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface windowSurface;
    EGLSurface parkingSurface;
    GLuint copyFramebuffer;

    int recommendedWidth;
    int recommendedHeight;
    XrQuaternionf headOrigin;
    QuestOpenXR::HeadState head;

    std::vector<XrViewConfigurationView> viewConfigs;
    std::vector<XrView> views;
    std::vector<XrCompositionLayerProjectionView> projectionViews;
    std::vector<Swapchain> swapchains;

    State()
        : attempted(false), initialized(false), sessionRunning(false),
          fatalError(false), parkingActive(false), parkingSurfaceless(false),
          headOriginSet(false), headValid(false), loggedWaitFrame(false),
          loggedBeginFrame(false), loggedEndFrame(false),
          ownsPresentation(false), exitRequested(false), ownerThread(-1),
          frameCount(0), logFile(NULL), instance(XR_NULL_HANDLE),
          systemId(XR_NULL_SYSTEM_ID), session(XR_NULL_HANDLE),
          appSpace(XR_NULL_HANDLE), sessionState(XR_SESSION_STATE_UNKNOWN),
          display(EGL_NO_DISPLAY), config(NULL), context(EGL_NO_CONTEXT),
          windowSurface(EGL_NO_SURFACE), parkingSurface(EGL_NO_SURFACE),
          copyFramebuffer(0), recommendedWidth(0), recommendedHeight(0) {
        headOrigin.x = headOrigin.y = headOrigin.z = 0.0f;
        headOrigin.w = 1.0f;
        head.valid = false;
        head.orientation[0] = head.orientation[1] = head.orientation[2] = 0.0f;
        head.orientation[3] = 1.0f;
        head.tanHalfFov[0] = head.tanHalfFov[1] = 1.0f;
    }
};

State g;

pid_t CurrentThreadId() {
    return static_cast<pid_t>(syscall(SYS_gettid));
}

uint64_t MonotonicMilliseconds() {
    timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return static_cast<uint64_t>(value.tv_sec) * 1000ULL +
           static_cast<uint64_t>(value.tv_nsec) / 1000000ULL;
}

void OpenPersistentLog() {
    const char* root = getenv("STORAGE_ROOT");
    if (root == NULL || root[0] == '\0') {
        __android_log_write(ANDROID_LOG_ERROR, kLogTag,
                            "STORAGE_ROOT is unavailable");
        return;
    }

    g.gameRoot = root;
    while (!g.gameRoot.empty() && g.gameRoot[g.gameRoot.size() - 1] == '/') {
        g.gameRoot.erase(g.gameRoot.size() - 1);
    }
    const std::string userFiles = g.gameRoot + "/userfiles";
    if (mkdir(userFiles.c_str(), 0775) != 0 && errno != EEXIST) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "Unable to create %s: errno=%d",
                            userFiles.c_str(), errno);
        return;
    }
    g.logFile = fopen((userFiles + "/xr_log.txt").c_str(), "a");
}

void Log(const char* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    __android_log_write(ANDROID_LOG_INFO, kLogTag, text);
    if (g.logFile != NULL) {
        fprintf(g.logFile, "%s\n", text);
        fflush(g.logFile);
    }
}

const char* SessionStateName(XrSessionState state) {
    switch (state) {
    case XR_SESSION_STATE_UNKNOWN: return "UNKNOWN";
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "INVALID";
    }
}

const char* ResultName(XrResult result) {
    static thread_local char name[XR_MAX_RESULT_STRING_SIZE];
    if (g.instance != XR_NULL_HANDLE &&
        XR_SUCCEEDED(xrResultToString(g.instance, result, name))) {
        return name;
    }
    snprintf(name, sizeof(name), "XrResult(%d)", result);
    return name;
}

bool Check(XrResult result, const char* operation) {
    if (XR_FAILED(result)) {
        Log("%s failed: %s", operation, ResultName(result));
        return false;
    }
    return true;
}

bool OnOwnerThread(const char* operation) {
    const pid_t current = CurrentThreadId();
    if (current == g.ownerThread) {
        return true;
    }
    Log("%s called on tid=%d; sole owner is SDLThread tid=%d", operation,
        static_cast<int>(current), static_cast<int>(g.ownerThread));
    g.fatalError = true;
    g.exitRequested.store(true);
    return false;
}

bool HasExtension(const std::vector<XrExtensionProperties>& extensions,
                  const char* name) {
    for (size_t i = 0; i < extensions.size(); ++i) {
        if (strcmp(extensions[i].extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

void SaveGlState(SavedGlState* state) {
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &state->drawFramebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &state->readFramebuffer);
    glGetIntegerv(GL_VIEWPORT, state->viewport);
    glGetIntegerv(GL_SCISSOR_BOX, state->scissor);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, state->clearColor);
    glGetBooleanv(GL_COLOR_WRITEMASK, state->colorMask);
    state->scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
}

void RestoreGlState(const SavedGlState& state) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                      static_cast<GLuint>(state.drawFramebuffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      static_cast<GLuint>(state.readFramebuffer));
    glViewport(state.viewport[0], state.viewport[1],
               state.viewport[2], state.viewport[3]);
    glScissor(state.scissor[0], state.scissor[1],
              state.scissor[2], state.scissor[3]);
    glClearColor(state.clearColor[0], state.clearColor[1],
                 state.clearColor[2], state.clearColor[3]);
    glColorMask(state.colorMask[0], state.colorMask[1],
                state.colorMask[2], state.colorMask[3]);
    if (state.scissorEnabled == GL_TRUE) {
        glEnable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

XrQuaternionf Normalize(const XrQuaternionf& value) {
    const float length = sqrtf(value.x * value.x + value.y * value.y +
                               value.z * value.z + value.w * value.w);
    if (length <= 0.00001f) {
        XrQuaternionf identity = {0.0f, 0.0f, 0.0f, 1.0f};
        return identity;
    }
    XrQuaternionf result = {value.x / length, value.y / length,
                            value.z / length, value.w / length};
    return result;
}

XrQuaternionf Conjugate(const XrQuaternionf& value) {
    XrQuaternionf result = {-value.x, -value.y, -value.z, value.w};
    return result;
}

XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b) {
    XrQuaternionf result = {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
    };
    return result;
}

void UpdateHeadState(uint32_t viewCount) {
    if (viewCount == 0) {
        return;
    }
    const XrQuaternionf current = Normalize(g.views[0].pose.orientation);
    if (!g.headOriginSet) {
        g.headOrigin = current;
        g.headOriginSet = true;
        Log("Head orientation origin captured from first tracked frame");
    }
    const XrQuaternionf relative =
            Normalize(Multiply(Conjugate(g.headOrigin), current));
    g.head.orientation[0] = relative.x;
    g.head.orientation[1] = relative.y;
    g.head.orientation[2] = relative.z;
    g.head.orientation[3] = relative.w;

    float horizontal = 0.0f;
    float vertical = 0.0f;
    for (uint32_t i = 0; i < viewCount; ++i) {
        horizontal = fmaxf(horizontal, fabsf(tanf(g.views[i].fov.angleLeft)));
        horizontal = fmaxf(horizontal, fabsf(tanf(g.views[i].fov.angleRight)));
        vertical = fmaxf(vertical, fabsf(tanf(g.views[i].fov.angleUp)));
        vertical = fmaxf(vertical, fabsf(tanf(g.views[i].fov.angleDown)));
    }
    g.head.tanHalfFov[0] = horizontal > 0.01f ? horizontal : 1.0f;
    g.head.tanHalfFov[1] = vertical > 0.01f ? vertical : 1.0f;
    g.head.valid = true;
    g.headValid = true;
}

bool InitializeLoader() {
    if (javaVM == NULL || g_pJavaWrapper == NULL ||
        g_pJavaWrapper->activity == NULL) {
        Log("OpenXR loader cannot initialize without GameActivity/JavaVM");
        return false;
    }

    PFN_xrInitializeLoaderKHR initializeLoader = NULL;
    const XrResult procResult = xrGetInstanceProcAddr(
            XR_NULL_HANDLE, "xrInitializeLoaderKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader));
    if (XR_FAILED(procResult) || initializeLoader == NULL) {
        Log("xrInitializeLoaderKHR unavailable (%d); using Android instance info",
            procResult);
        return true;
    }

    XrLoaderInitInfoAndroidKHR info = {
        XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR
    };
    info.applicationVM = javaVM;
    info.applicationContext = g_pJavaWrapper->activity;
    if (!Check(initializeLoader(
            reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&info)),
            "xrInitializeLoaderKHR")) {
        return false;
    }
    Log("xrInitializeLoaderKHR succeeded");
    return true;
}

bool CreateInstanceAndSystem() {
    uint32_t extensionCount = 0;
    if (!Check(xrEnumerateInstanceExtensionProperties(
            NULL, 0, &extensionCount, NULL),
            "xrEnumerateInstanceExtensionProperties(count)")) {
        return false;
    }
    std::vector<XrExtensionProperties> extensions(
            extensionCount, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    if (!Check(xrEnumerateInstanceExtensionProperties(
            NULL, extensionCount, &extensionCount, extensions.data()),
            "xrEnumerateInstanceExtensionProperties(list)")) {
        return false;
    }

    const char* required[] = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        if (!HasExtension(extensions, required[i])) {
            Log("Required extension missing: %s", required[i]);
            return false;
        }
    }

    XrInstanceCreateInfoAndroidKHR androidInfo = {
        XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR
    };
    androidInfo.applicationVM = javaVM;
    androidInfo.applicationActivity = g_pJavaWrapper->activity;

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.next = &androidInfo;
    strncpy(createInfo.applicationInfo.applicationName, "reVC Quest",
            XR_MAX_APPLICATION_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    strncpy(createInfo.applicationInfo.engineName, "reVC librw SDL",
            XR_MAX_ENGINE_NAME_SIZE - 1);
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    createInfo.enabledExtensionCount = 2;
    createInfo.enabledExtensionNames = required;
    if (!Check(xrCreateInstance(&createInfo, &g.instance), "xrCreateInstance")) {
        return false;
    }
    Log("xrCreateInstance succeeded");

    XrInstanceProperties properties = {XR_TYPE_INSTANCE_PROPERTIES};
    if (Check(xrGetInstanceProperties(g.instance, &properties),
              "xrGetInstanceProperties")) {
        Log("Runtime=%s version=%u.%u.%u", properties.runtimeName,
            XR_VERSION_MAJOR(properties.runtimeVersion),
            XR_VERSION_MINOR(properties.runtimeVersion),
            XR_VERSION_PATCH(properties.runtimeVersion));
    }

    XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Check(xrGetSystem(g.instance, &systemInfo, &g.systemId),
               "xrGetSystem")) {
        return false;
    }
    Log("xrGetSystem succeeded: systemId=%llu",
        static_cast<unsigned long long>(g.systemId));
    return true;
}

bool SupportsSurfaceless(EGLDisplay display) {
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    return extensions != NULL &&
           strstr(extensions, "EGL_KHR_surfaceless_context") != NULL;
}

bool CaptureAndParkSdlContext() {
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = NULL;
    if (!Check(xrGetInstanceProcAddr(
            g.instance, "xrGetOpenGLESGraphicsRequirementsKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)),
            "xrGetInstanceProcAddr(xrGetOpenGLESGraphicsRequirementsKHR)") ||
        getRequirements == NULL) {
        return false;
    }
    XrGraphicsRequirementsOpenGLESKHR requirements = {
        XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR
    };
    if (!Check(getRequirements(g.instance, g.systemId, &requirements),
               "xrGetOpenGLESGraphicsRequirementsKHR")) {
        return false;
    }

    g.display = eglGetCurrentDisplay();
    g.context = eglGetCurrentContext();
    g.windowSurface = eglGetCurrentSurface(EGL_DRAW);
    if (g.display == EGL_NO_DISPLAY || g.context == EGL_NO_CONTEXT ||
        g.windowSurface == EGL_NO_SURFACE) {
        Log("SDL EGL binding missing: display=%p context=%p surface=%p",
            g.display, g.context, g.windowSurface);
        return false;
    }

    EGLint configId = 0;
    if (eglQueryContext(g.display, g.context, EGL_CONFIG_ID, &configId) !=
        EGL_TRUE) {
        Log("eglQueryContext(EGL_CONFIG_ID) failed: 0x%x", eglGetError());
        return false;
    }
    const EGLint configAttributes[] = {EGL_CONFIG_ID, configId, EGL_NONE};
    EGLint configCount = 0;
    if (eglChooseConfig(g.display, configAttributes, &g.config, 1,
                        &configCount) != EGL_TRUE || configCount != 1) {
        Log("Unable to resolve SDL EGLConfig id=%d: 0x%x", configId,
            eglGetError());
        return false;
    }

    GLint major = 0;
    GLint minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    const XrVersion glVersion = XR_MAKE_VERSION(major, minor, 0);
    if (glVersion < requirements.minApiVersionSupported ||
        glVersion > requirements.maxApiVersionSupported) {
        Log("GLES %d.%d outside runtime range %u.%u-%u.%u", major, minor,
            XR_VERSION_MAJOR(requirements.minApiVersionSupported),
            XR_VERSION_MINOR(requirements.minApiVersionSupported),
            XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
            XR_VERSION_MINOR(requirements.maxApiVersionSupported));
        return false;
    }
    Log("Sole graphics owner tid=%d EGLContext=%p GLES=%d.%d renderer=%s",
        static_cast<int>(g.ownerThread), g.context, major, minor,
        reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    // Present one ordinary Android frame before Quest retires the temporary
    // window. This is still the same SDL context and is not an XR renderer.
    EGLint windowWidth = 0;
    EGLint windowHeight = 0;
    if (eglQuerySurface(g.display, g.windowSurface, EGL_WIDTH, &windowWidth) ==
            EGL_TRUE &&
        eglQuerySurface(g.display, g.windowSurface, EGL_HEIGHT, &windowHeight) ==
            EGL_TRUE && windowWidth > 0 && windowHeight > 0) {
        SavedGlState saved;
        SaveGlState(&saved);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, windowWidth, windowHeight);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        if (eglSwapBuffers(g.display, g.windowSurface) == EGL_TRUE) {
            Log("SDL Android bootstrap frame presented (%dx%d)",
                windowWidth, windowHeight);
        } else {
            Log("SDL Android bootstrap swap unavailable: 0x%x", eglGetError());
        }
        RestoreGlState(saved);
    }

    EGLint surfaceTypes = 0;
    if (eglGetConfigAttrib(g.display, g.config, EGL_SURFACE_TYPE,
                           &surfaceTypes) == EGL_TRUE &&
        (surfaceTypes & EGL_PBUFFER_BIT) != 0) {
        const EGLint pbufferAttributes[] = {
            EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE
        };
        g.parkingSurface = eglCreatePbufferSurface(
                g.display, g.config, pbufferAttributes);
        if (g.parkingSurface != EGL_NO_SURFACE &&
            eglMakeCurrent(g.display, g.parkingSurface, g.parkingSurface,
                           g.context) == EGL_TRUE) {
            g.parkingActive = true;
            g.parkingSurfaceless = false;
            Log("Same SDL EGLContext rebound to pbuffer=%p", g.parkingSurface);
            return true;
        }
        if (g.parkingSurface != EGL_NO_SURFACE) {
            eglDestroySurface(g.display, g.parkingSurface);
            g.parkingSurface = EGL_NO_SURFACE;
        }
    }

    if (SupportsSurfaceless(g.display) &&
        eglMakeCurrent(g.display, EGL_NO_SURFACE, EGL_NO_SURFACE, g.context) ==
            EGL_TRUE) {
        g.parkingActive = true;
        g.parkingSurfaceless = true;
        Log("Same SDL EGLContext rebound to surfaceless EGL");
        return true;
    }

    eglMakeCurrent(g.display, g.windowSurface, g.windowSurface, g.context);
    Log("Unable to retain SDL EGLContext off the Android window: 0x%x",
        eglGetError());
    return false;
}

bool CreateSessionAndSpace() {
    XrGraphicsBindingOpenGLESAndroidKHR binding = {
        XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR
    };
    binding.display = g.display;
    binding.config = g.config;
    binding.context = g.context;

    // From here onward Quest may retire the Activity's window. Java retains
    // this exact SDL thread/context while this flag is true.
    g.ownsPresentation.store(true);

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = g.systemId;
    if (!Check(xrCreateSession(g.instance, &sessionInfo, &g.session),
               "xrCreateSession")) {
        g.ownsPresentation.store(false);
        return false;
    }
    Log("xrCreateSession succeeded on SDLThread tid=%d",
        static_cast<int>(g.ownerThread));

    XrReferenceSpaceCreateInfo spaceInfo = {
        XR_TYPE_REFERENCE_SPACE_CREATE_INFO
    };
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    XrResult result = xrCreateReferenceSpace(g.session, &spaceInfo, &g.appSpace);
    if (XR_FAILED(result)) {
        Log("LOCAL space unavailable (%s); using VIEW", ResultName(result));
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        if (!Check(xrCreateReferenceSpace(g.session, &spaceInfo, &g.appSpace),
                   "xrCreateReferenceSpace(VIEW)")) {
            return false;
        }
    }
    return true;
}

int64_t SelectColorFormat(const std::vector<int64_t>& formats) {
    const int64_t preferred[] = {GL_RGBA8, GL_SRGB8_ALPHA8, GL_RGB10_A2};
    for (size_t p = 0; p < sizeof(preferred) / sizeof(preferred[0]); ++p) {
        for (size_t i = 0; i < formats.size(); ++i) {
            if (formats[i] == preferred[p]) {
                return preferred[p];
            }
        }
    }
    return formats.empty() ? 0 : formats[0];
}

bool CreateSwapchains() {
    uint32_t viewCount = 0;
    if (!Check(xrEnumerateViewConfigurationViews(
            g.instance, g.systemId, kViewType, 0, &viewCount, NULL),
            "xrEnumerateViewConfigurationViews(count)") || viewCount == 0) {
        return false;
    }
    g.viewConfigs.assign(
            viewCount, XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (!Check(xrEnumerateViewConfigurationViews(
            g.instance, g.systemId, kViewType, viewCount, &viewCount,
            g.viewConfigs.data()),
            "xrEnumerateViewConfigurationViews(list)")) {
        return false;
    }

    uint32_t formatCount = 0;
    if (!Check(xrEnumerateSwapchainFormats(
            g.session, 0, &formatCount, NULL),
            "xrEnumerateSwapchainFormats(count)")) {
        return false;
    }
    std::vector<int64_t> formats(formatCount);
    if (!Check(xrEnumerateSwapchainFormats(
            g.session, formatCount, &formatCount, formats.data()),
            "xrEnumerateSwapchainFormats(list)")) {
        return false;
    }
    const int64_t format = SelectColorFormat(formats);
    if (format == 0) {
        Log("Runtime exposed no color swapchain format");
        return false;
    }

    g.views.assign(viewCount, XrView{XR_TYPE_VIEW});
    g.projectionViews.assign(
            viewCount, XrCompositionLayerProjectionView{
                    XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
    g.swapchains.resize(viewCount);
    g.recommendedWidth = 0;
    g.recommendedHeight = 0;

    for (uint32_t eye = 0; eye < viewCount; ++eye) {
        Swapchain& swapchain = g.swapchains[eye];
        swapchain.width = static_cast<int32_t>(
                g.viewConfigs[eye].recommendedImageRectWidth);
        swapchain.height = static_cast<int32_t>(
                g.viewConfigs[eye].recommendedImageRectHeight);
        g.recommendedWidth = g.recommendedWidth > swapchain.width
                ? g.recommendedWidth : swapchain.width;
        g.recommendedHeight = g.recommendedHeight > swapchain.height
                ? g.recommendedHeight : swapchain.height;

        XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                          XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = format;
        info.sampleCount = 1;
        info.width = swapchain.width;
        info.height = swapchain.height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (!Check(xrCreateSwapchain(g.session, &info, &swapchain.handle),
                   "xrCreateSwapchain")) {
            return false;
        }

        uint32_t imageCount = 0;
        if (!Check(xrEnumerateSwapchainImages(
                swapchain.handle, 0, &imageCount, NULL),
                "xrEnumerateSwapchainImages(count)")) {
            return false;
        }
        swapchain.images.assign(
                imageCount, XrSwapchainImageOpenGLESKHR{
                        XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        if (!Check(xrEnumerateSwapchainImages(
                swapchain.handle, imageCount, &imageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(
                        swapchain.images.data())),
                "xrEnumerateSwapchainImages(list)")) {
            return false;
        }
        Log("Eye %u swapchain size=%dx%d images=%u", eye,
            swapchain.width, swapchain.height, imageCount);
    }

    glGenFramebuffers(1, &g.copyFramebuffer);
    if (g.copyFramebuffer == 0) {
        Log("glGenFramebuffers for OpenXR copy failed: 0x%x", glGetError());
        return false;
    }
    Log("Swapchains ready; reVC source target=%dx%d", g.recommendedWidth,
        g.recommendedHeight);
    return true;
}

bool PollEventsInternal() {
    if (g.instance == XR_NULL_HANDLE) {
        return true;
    }
    XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
    for (;;) {
        const XrResult result = xrPollEvent(g.instance, &event);
        if (result == XR_EVENT_UNAVAILABLE) {
            return true;
        }
        if (XR_FAILED(result)) {
            Log("xrPollEvent failed: %s", ResultName(result));
            g.fatalError = true;
            g.exitRequested.store(true);
            return false;
        }

        if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            Log("OpenXR instance loss pending");
            g.fatalError = true;
            g.exitRequested.store(true);
        } else if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged* changed =
                    reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            const XrSessionState previous = g.sessionState;
            g.sessionState = changed->state;
            Log("OpenXR session %s -> %s", SessionStateName(previous),
                SessionStateName(g.sessionState));

            if (g.sessionState == XR_SESSION_STATE_READY &&
                !g.sessionRunning) {
                XrSessionBeginInfo begin = {XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = kViewType;
                if (!Check(xrBeginSession(g.session, &begin),
                           "xrBeginSession")) {
                    g.fatalError = true;
                    g.exitRequested.store(true);
                    return false;
                }
                g.sessionRunning = true;
                Log("xrBeginSession succeeded after READY");
            } else if (g.sessionState == XR_SESSION_STATE_STOPPING &&
                       g.sessionRunning) {
                if (!Check(xrEndSession(g.session), "xrEndSession")) {
                    g.fatalError = true;
                    g.exitRequested.store(true);
                    return false;
                }
                g.sessionRunning = false;
                Log("xrEndSession succeeded after STOPPING");
            } else if (g.sessionState == XR_SESSION_STATE_EXITING) {
                Log("OpenXR runtime requested clean Activity exit");
                g.exitRequested.store(true);
            } else if (g.sessionState == XR_SESSION_STATE_LOSS_PENDING) {
                Log("OpenXR session loss pending");
                g.fatalError = true;
                g.exitRequested.store(true);
            }
        }
        event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
}

bool BlitGameRasterToEye(const Swapchain& swapchain, uint32_t eye,
                         GLuint sourceFramebuffer, int sourceWidth,
                         int sourceHeight) {
    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquire = {
        XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO
    };
    if (!Check(xrAcquireSwapchainImage(
            swapchain.handle, &acquire, &imageIndex),
            "xrAcquireSwapchainImage")) {
        return false;
    }

    XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (!Check(xrWaitSwapchainImage(swapchain.handle, &wait),
               "xrWaitSwapchainImage")) {
        XrSwapchainImageReleaseInfo release = {
            XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO
        };
        xrReleaseSwapchainImage(swapchain.handle, &release);
        return false;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFramebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.copyFramebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D,
                           swapchain.images[imageIndex].image, 0);

    bool valid = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) ==
                         GL_FRAMEBUFFER_COMPLETE &&
                 glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) ==
                         GL_FRAMEBUFFER_COMPLETE;
    if (valid) {
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glViewport(0, 0, swapchain.width, swapchain.height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        int destinationWidth = swapchain.width;
        int destinationHeight = swapchain.height;
        int destinationX = 0;
        int destinationY = 0;
        const float sourceAspect = static_cast<float>(sourceWidth) /
                                   static_cast<float>(sourceHeight);
        const float targetAspect = static_cast<float>(swapchain.width) /
                                   static_cast<float>(swapchain.height);
        if (sourceAspect > targetAspect) {
            destinationHeight = static_cast<int>(
                    static_cast<float>(destinationWidth) / sourceAspect);
            destinationY = (swapchain.height - destinationHeight) / 2;
        } else if (sourceAspect < targetAspect) {
            destinationWidth = static_cast<int>(
                    static_cast<float>(destinationHeight) * sourceAspect);
            destinationX = (swapchain.width - destinationWidth) / 2;
        }

        glBlitFramebuffer(0, 0, sourceWidth, sourceHeight,
                          destinationX, destinationY,
                          destinationX + destinationWidth,
                          destinationY + destinationHeight,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glFlush();
        const GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            Log("Eye %u game-raster blit failed: GL=0x%x", eye, error);
            valid = false;
        }
    } else {
        Log("Eye %u framebuffer incomplete: read=0x%x draw=0x%x", eye,
            glCheckFramebufferStatus(GL_READ_FRAMEBUFFER),
            glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER));
    }

    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, 0, 0);
    XrSwapchainImageReleaseInfo release = {
        XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO
    };
    if (!Check(xrReleaseSwapchainImage(swapchain.handle, &release),
               "xrReleaseSwapchainImage")) {
        valid = false;
    }
    return valid;
}

bool RenderFrame(GLuint sourceFramebuffer, int sourceWidth,
                 int sourceHeight) {
    XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState = {XR_TYPE_FRAME_STATE};
    if (!Check(xrWaitFrame(g.session, &waitInfo, &frameState),
               "xrWaitFrame")) {
        return false;
    }
    if (!g.loggedWaitFrame) {
        Log("xrWaitFrame succeeded: shouldRender=%d", frameState.shouldRender);
        g.loggedWaitFrame = true;
    }

    XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
    if (!Check(xrBeginFrame(g.session, &beginInfo), "xrBeginFrame")) {
        return false;
    }
    if (!g.loggedBeginFrame) {
        Log("xrBeginFrame succeeded");
        g.loggedBeginFrame = true;
    }

    XrCompositionLayerProjection layer = {
        XR_TYPE_COMPOSITION_LAYER_PROJECTION
    };
    const XrCompositionLayerBaseHeader* layers[1] = {NULL};
    uint32_t layerCount = 0;
    bool renderValid = true;

    SavedGlState saved;
    SaveGlState(&saved);
    while (glGetError() != GL_NO_ERROR) {
        // Only errors produced by this bridge determine whether submission failed.
    }

    if (frameState.shouldRender == XR_TRUE) {
        XrViewLocateInfo locate = {XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = kViewType;
        locate.displayTime = frameState.predictedDisplayTime;
        locate.space = g.appSpace;
        XrViewState viewState = {XR_TYPE_VIEW_STATE};
        uint32_t viewCount = 0;
        const XrResult locateResult = xrLocateViews(
                g.session, &locate, &viewState,
                static_cast<uint32_t>(g.views.size()), &viewCount,
                g.views.data());
        const XrViewStateFlags required =
                XR_VIEW_STATE_ORIENTATION_VALID_BIT |
                XR_VIEW_STATE_POSITION_VALID_BIT;
        if (XR_SUCCEEDED(locateResult) &&
            (viewState.viewStateFlags & required) == required &&
            viewCount == g.swapchains.size()) {
            UpdateHeadState(viewCount);
            for (uint32_t eye = 0; eye < viewCount; ++eye) {
                renderValid = BlitGameRasterToEye(
                        g.swapchains[eye], eye, sourceFramebuffer,
                        sourceWidth, sourceHeight) && renderValid;
                XrCompositionLayerProjectionView& projection =
                        g.projectionViews[eye];
                projection = XrCompositionLayerProjectionView{
                        XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                projection.pose = g.views[eye].pose;
                projection.fov = g.views[eye].fov;
                projection.subImage.swapchain = g.swapchains[eye].handle;
                projection.subImage.imageRect.offset = {0, 0};
                projection.subImage.imageRect.extent = {
                    g.swapchains[eye].width, g.swapchains[eye].height
                };
            }
            if (renderValid) {
                layer.space = g.appSpace;
                layer.viewCount = viewCount;
                layer.views = g.projectionViews.data();
                layers[0] = reinterpret_cast<
                        const XrCompositionLayerBaseHeader*>(&layer);
                layerCount = 1;
            }
        } else if (XR_FAILED(locateResult)) {
            Log("xrLocateViews failed: %s", ResultName(locateResult));
            renderValid = false;
        } else if (g.frameCount < 5 || g.frameCount % 300 == 0) {
            Log("Tracked views not ready: flags=0x%llx count=%u",
                static_cast<unsigned long long>(viewState.viewStateFlags),
                viewCount);
        }
    }

    RestoreGlState(saved);

    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layerCount == 0 ? NULL : layers;
    if (!Check(xrEndFrame(g.session, &endInfo), "xrEndFrame")) {
        return false;
    }
    if (!g.loggedEndFrame) {
        Log("xrEndFrame succeeded: reVC layerCount=%u", layerCount);
        g.loggedEndFrame = true;
    }
    ++g.frameCount;
    if (g.frameCount == 1 || g.frameCount % 300 == 0) {
        Log("reVC game frames submitted=%llu state=%s",
            static_cast<unsigned long long>(g.frameCount),
            SessionStateName(g.sessionState));
    }
    return renderValid;
}

void DestroyRuntimeObjects() {
    g.ownsPresentation.store(false);
    if (g.copyFramebuffer != 0 && eglGetCurrentContext() == g.context) {
        glDeleteFramebuffers(1, &g.copyFramebuffer);
    }
    g.copyFramebuffer = 0;

    g.sessionRunning = false;
    if (g.appSpace != XR_NULL_HANDLE) {
        xrDestroySpace(g.appSpace);
        g.appSpace = XR_NULL_HANDLE;
    }
    for (size_t i = 0; i < g.swapchains.size(); ++i) {
        if (g.swapchains[i].handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(g.swapchains[i].handle);
            g.swapchains[i].handle = XR_NULL_HANDLE;
        }
    }
    g.swapchains.clear();
    g.views.clear();
    g.viewConfigs.clear();
    g.projectionViews.clear();
    if (g.session != XR_NULL_HANDLE) {
        xrDestroySession(g.session);
        g.session = XR_NULL_HANDLE;
        Log("xrDestroySession completed");
    }
    if (g.instance != XR_NULL_HANDLE) {
        xrDestroyInstance(g.instance);
        g.instance = XR_NULL_HANDLE;
        Log("xrDestroyInstance completed");
    }
    g.systemId = XR_NULL_SYSTEM_ID;
    g.sessionState = XR_SESSION_STATE_UNKNOWN;
    g.initialized = false;
}

void RestoreWindowIfAvailable() {
    if (!g.parkingActive || g.parkingSurfaceless ||
        g.parkingSurface == EGL_NO_SURFACE) {
        return;
    }
    if (g.display != EGL_NO_DISPLAY && g.context != EGL_NO_CONTEXT &&
        g.windowSurface != EGL_NO_SURFACE &&
        eglMakeCurrent(g.display, g.windowSurface, g.windowSurface,
                       g.context) == EGL_TRUE) {
        eglDestroySurface(g.display, g.parkingSurface);
        g.parkingSurface = EGL_NO_SURFACE;
        g.parkingActive = false;
        Log("SDL EGLContext restored to Android window for engine shutdown");
    } else {
        Log("Android window unavailable at shutdown; engine will release the "
            "SDL context while its pbuffer remains current");
    }
}

void MarkInitializationFailure(const char* stage) {
    Log("OpenXR initialization failed at %s; exiting GameActivity cleanly", stage);
    g.fatalError = true;
    g.exitRequested.store(true);
    DestroyRuntimeObjects();
    RestoreWindowIfAvailable();
}

} // namespace

namespace QuestOpenXR {

bool Initialize() {
    if (g.attempted) {
        return g.initialized;
    }
    g.attempted = true;
    g.ownerThread = CurrentThreadId();
    OpenPersistentLog();
    Log("=== reVC direct game-frame OpenXR startup ===");
    Log("Ownership: GameActivity -> SDLThread tid=%d -> SDL EGLContext -> OpenXR",
        static_cast<int>(g.ownerThread));
    Log("Game root=%s", g.gameRoot.empty() ? "<missing>" : g.gameRoot.c_str());

    if (!InitializeLoader()) {
        MarkInitializationFailure("Android loader");
        return false;
    }
    if (!CreateInstanceAndSystem()) {
        MarkInitializationFailure("instance/system");
        return false;
    }
    if (!CaptureAndParkSdlContext()) {
        MarkInitializationFailure("existing SDL EGLContext");
        return false;
    }
    if (!CreateSessionAndSpace()) {
        MarkInitializationFailure("session/reference space");
        return false;
    }
    if (!CreateSwapchains()) {
        MarkInitializationFailure("eye swapchains");
        return false;
    }

    g.initialized = true;
    Log("OpenXR objects ready; waiting for runtime READY event");
    PollEventsInternal();
    return !g.fatalError;
}

bool AwaitSessionReady(uint32_t timeoutMilliseconds) {
    if (!g.initialized || !OnOwnerThread("AwaitSessionReady")) {
        return false;
    }
    const uint64_t start = MonotonicMilliseconds();
    while (!g.sessionRunning && !g.exitRequested.load() && !g.fatalError) {
        if (!PollEventsInternal()) {
            return false;
        }
        if (MonotonicMilliseconds() - start >= timeoutMilliseconds) {
            Log("Runtime did not reach READY within %u ms; exiting instead of "
                "leaving Quest in a loading state", timeoutMilliseconds);
            g.fatalError = true;
            g.exitRequested.store(true);
            return false;
        }
        const timespec pause = {0, 2000000};
        nanosleep(&pause, NULL);
    }
    return g.sessionRunning && !g.fatalError;
}

FrameResult SubmitGameFrame(unsigned int sourceFramebuffer,
                            int sourceWidth, int sourceHeight) {
    if (!g.initialized) {
        return FrameResult::Inactive;
    }
    if (!OnOwnerThread("SubmitGameFrame")) {
        return FrameResult::FatalError;
    }
    if (!PollEventsInternal() || g.fatalError) {
        return FrameResult::FatalError;
    }
    if (g.exitRequested.load()) {
        return FrameResult::ExitRequested;
    }
    if (!g.sessionRunning) {
        return FrameResult::WaitingForSession;
    }
    if (sourceFramebuffer == 0 || sourceWidth <= 0 || sourceHeight <= 0) {
        Log("Invalid reVC source framebuffer=%u size=%dx%d",
            sourceFramebuffer, sourceWidth, sourceHeight);
        g.fatalError = true;
        g.exitRequested.store(true);
        return FrameResult::FatalError;
    }
    if (!RenderFrame(static_cast<GLuint>(sourceFramebuffer),
                     sourceWidth, sourceHeight)) {
        Log("Direct reVC game-frame submission failed; requesting clean exit");
        g.fatalError = true;
        g.exitRequested.store(true);
        return FrameResult::FatalError;
    }
    return FrameResult::Presented;
}

void PollEvents() {
    if (g.initialized && !g.fatalError && OnOwnerThread("PollEvents")) {
        PollEventsInternal();
    }
}

void Shutdown() {
    if (!g.attempted) {
        return;
    }
    if (g.ownerThread != -1 && !OnOwnerThread("Shutdown")) {
        return;
    }
    Log("OpenXR shutdown: state=%s running=%d submitted=%llu",
        SessionStateName(g.sessionState), g.sessionRunning ? 1 : 0,
        static_cast<unsigned long long>(g.frameCount));
    DestroyRuntimeObjects();
    RestoreWindowIfAvailable();
}

void FinalizeAfterEngineShutdown() {
    if (g.parkingSurface != EGL_NO_SURFACE && g.display != EGL_NO_DISPLAY) {
        eglDestroySurface(g.display, g.parkingSurface);
        g.parkingSurface = EGL_NO_SURFACE;
    }
    g.parkingActive = false;
    g.parkingSurfaceless = false;
    Log("OpenXR and SDL graphics ownership fully released");
    if (g.logFile != NULL) {
        fclose(g.logFile);
        g.logFile = NULL;
    }
    g.display = EGL_NO_DISPLAY;
    g.config = NULL;
    g.context = EGL_NO_CONTEXT;
    g.windowSurface = EGL_NO_SURFACE;
    g.ownerThread = -1;
}

int RecommendedWidth() {
    return g.recommendedWidth;
}

int RecommendedHeight() {
    return g.recommendedHeight;
}

bool GetHeadState(HeadState* state) {
    if (state == NULL || !g.headValid) {
        return false;
    }
    *state = g.head;
    return true;
}

bool OwnsPresentation() {
    return g.ownsPresentation.load();
}

bool ExitRequested() {
    return g.exitRequested.load();
}

} // namespace QuestOpenXR

extern "C" JNIEXPORT jboolean JNICALL
Java_com_revc_game_core_REVC_isOpenXrPresentationActive(JNIEnv*, jclass) {
    return QuestOpenXR::OwnsPresentation() && !QuestOpenXR::ExitRequested()
            ? JNI_TRUE : JNI_FALSE;
}

#endif
