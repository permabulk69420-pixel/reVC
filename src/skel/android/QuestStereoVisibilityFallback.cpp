#if defined(ANDROID)

#include <GLES3/gl3.h>
#include <android/log.h>
#include <cmath>
#include <cstdint>

#include "QuestOpenXR.h"

namespace {

const char* const kTag = "reVC-XR";
GLuint gSavedLeftTexture = 0;
GLuint gSavedLeftFramebuffer = 0;
int gSavedLeftWidth = 0;
int gSavedLeftHeight = 0;
bool gSavedLeftValid = false;
bool gLoggedFallback = false;

bool EnsureSavedLeftTarget(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (gSavedLeftTexture != 0 && gSavedLeftFramebuffer != 0 &&
        gSavedLeftWidth == width && gSavedLeftHeight == height) {
        return true;
    }

    if (gSavedLeftFramebuffer != 0) {
        glDeleteFramebuffers(1, &gSavedLeftFramebuffer);
        gSavedLeftFramebuffer = 0;
    }
    if (gSavedLeftTexture != 0) {
        glDeleteTextures(1, &gSavedLeftTexture);
        gSavedLeftTexture = 0;
    }

    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture = 0;
    GLint previousDrawFramebuffer = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);

    glGenTextures(1, &gSavedLeftTexture);
    glBindTexture(GL_TEXTURE_2D, gSavedLeftTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glGenFramebuffers(1, &gSavedLeftFramebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gSavedLeftFramebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, gSavedLeftTexture, 0);
    const bool complete =
            glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) ==
            GL_FRAMEBUFFER_COMPLETE;

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                      static_cast<GLuint>(previousDrawFramebuffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));

    if (!complete || glGetError() != GL_NO_ERROR) {
        if (gSavedLeftFramebuffer != 0) {
            glDeleteFramebuffers(1, &gSavedLeftFramebuffer);
            gSavedLeftFramebuffer = 0;
        }
        if (gSavedLeftTexture != 0) {
            glDeleteTextures(1, &gSavedLeftTexture);
            gSavedLeftTexture = 0;
        }
        return false;
    }

    gSavedLeftWidth = width;
    gSavedLeftHeight = height;
    return true;
}

bool SaveLeftEye(unsigned int sourceFramebuffer, int width, int height) {
    gSavedLeftValid = false;
    if (sourceFramebuffer == 0 || !EnsureSavedLeftTarget(width, height)) {
        return false;
    }

    GLint previousReadFramebuffer = 0;
    GLint previousDrawFramebuffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);

    while (glGetError() != GL_NO_ERROR) {
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFramebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gSavedLeftFramebuffer);
    glBlitFramebuffer(0, 0, width, height,
                      0, 0, width, height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glFlush();
    const bool copied = glGetError() == GL_NO_ERROR;

    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      static_cast<GLuint>(previousReadFramebuffer));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                      static_cast<GLuint>(previousDrawFramebuffer));
    gSavedLeftValid = copied;
    return copied;
}

} // namespace

extern "C" QuestOpenXR::StereoFrameResult
__real__ZN11QuestOpenXR16BeginStereoFrameEPNS_7EyeViewEjPj(
        QuestOpenXR::EyeView*, uint32_t, uint32_t*);
extern "C" bool
__real__ZN11QuestOpenXR15SubmitStereoEyeEjjii(
        uint32_t, unsigned int, int, int);

extern "C" QuestOpenXR::StereoFrameResult
__wrap__ZN11QuestOpenXR16BeginStereoFrameEPNS_7EyeViewEjPj(
        QuestOpenXR::EyeView* eyes, uint32_t eyeCapacity,
        uint32_t* eyeCount) {
    const QuestOpenXR::StereoFrameResult result =
            __real__ZN11QuestOpenXR16BeginStereoFrameEPNS_7EyeViewEjPj(
                    eyes, eyeCapacity, eyeCount);
    if (result != QuestOpenXR::StereoFrameResult::Render || eyes == NULL ||
        eyeCount == NULL || *eyeCount == 0 || eyeCapacity < *eyeCount) {
        return result;
    }

    float horizontal = 0.0f;
    float vertical = 0.0f;
    for (uint32_t eye = 0; eye < *eyeCount; ++eye) {
        horizontal = fmaxf(horizontal, fabsf(tanf(eyes[eye].angleLeft)));
        horizontal = fmaxf(horizontal, fabsf(tanf(eyes[eye].angleRight)));
        vertical = fmaxf(vertical, fabsf(tanf(eyes[eye].angleUp)));
        vertical = fmaxf(vertical, fabsf(tanf(eyes[eye].angleDown)));
    }
    horizontal = horizontal > 0.01f ? horizontal : 1.0f;
    vertical = vertical > 0.01f ? vertical : 1.0f;
    const float horizontalAngle = atanf(horizontal);
    const float verticalAngle = atanf(vertical);

    for (uint32_t eye = 0; eye < *eyeCount; ++eye) {
        // Keep the already-proven tracked orientation while removing the two
        // new variables most likely to invalidate the RenderWare world pass:
        // positional conversion and asymmetric view offsets.
        eyes[eye].position[0] = 0.0f;
        eyes[eye].position[1] = 0.0f;
        eyes[eye].position[2] = 0.0f;
        eyes[eye].angleLeft = -horizontalAngle;
        eyes[eye].angleRight = horizontalAngle;
        eyes[eye].angleDown = -verticalAngle;
        eyes[eye].angleUp = verticalAngle;
    }
    return result;
}

extern "C" bool
__wrap__ZN11QuestOpenXR15SubmitStereoEyeEjjii(
        uint32_t eye, unsigned int sourceFramebuffer,
        int sourceWidth, int sourceHeight) {
    if (eye == 0) {
        const bool submitted =
                __real__ZN11QuestOpenXR15SubmitStereoEyeEjjii(
                        eye, sourceFramebuffer, sourceWidth, sourceHeight);
        if (submitted) {
            SaveLeftEye(sourceFramebuffer, sourceWidth, sourceHeight);
        }
        return submitted;
    }

    if (eye == 1 && gSavedLeftValid) {
        if (!gLoggedFallback) {
            __android_log_write(ANDROID_LOG_INFO, kTag,
                    "Visibility fallback active: preserved left render feeds both eyes while world replay is repaired");
            gLoggedFallback = true;
        }
        return __real__ZN11QuestOpenXR15SubmitStereoEyeEjjii(
                eye, gSavedLeftFramebuffer,
                gSavedLeftWidth, gSavedLeftHeight);
    }

    return __real__ZN11QuestOpenXR15SubmitStereoEyeEjjii(
            eye, sourceFramebuffer, sourceWidth, sourceHeight);
}

#endif
