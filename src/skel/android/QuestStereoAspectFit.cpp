#if defined(ANDROID)

#include <android/log.h>
#include <GLES3/gl3.h>

#include <cmath>

#include "QuestOpenXR.h"

namespace {

bool gLoggedAspectFit = false;

} // namespace

extern "C" void __real_glBlitFramebuffer(
        GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
        GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
        GLbitfield mask, GLenum filter);

extern "C" void __wrap_glBlitFramebuffer(
        GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
        GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
        GLbitfield mask, GLenum filter) {
    const int sourceWidth = std::abs(srcX1 - srcX0);
    const int sourceHeight = std::abs(srcY1 - srcY0);
    const int destinationWidth = std::abs(dstX1 - dstX0);
    const int destinationHeight = std::abs(dstY1 - dstY0);

    const bool likelyStereoPresentationCopy =
            QuestOpenXR::StereoFrameActive() &&
            mask == GL_COLOR_BUFFER_BIT && filter == GL_LINEAR &&
            srcX0 == 0 && srcY0 == 0 && dstX0 == 0 && dstY0 == 0 &&
            sourceWidth >= 1000 && sourceHeight >= 500 &&
            destinationWidth >= 1000 && destinationHeight >= 1000;

    if (likelyStereoPresentationCopy && sourceHeight > 0 &&
        destinationHeight > 0) {
        const float sourceAspect = static_cast<float>(sourceWidth) /
                                   static_cast<float>(sourceHeight);
        const float destinationAspect =
                static_cast<float>(destinationWidth) /
                static_cast<float>(destinationHeight);

        if (fabsf(sourceAspect - destinationAspect) > 0.05f) {
            GLint fittedX0 = dstX0;
            GLint fittedY0 = dstY0;
            GLint fittedX1 = dstX1;
            GLint fittedY1 = dstY1;

            if (sourceAspect > destinationAspect) {
                const int fittedHeight = static_cast<int>(lroundf(
                        static_cast<float>(destinationWidth) / sourceAspect));
                const int margin = (destinationHeight - fittedHeight) / 2;
                fittedY0 = dstY0 + margin;
                fittedY1 = fittedY0 + fittedHeight;
            } else {
                const int fittedWidth = static_cast<int>(lroundf(
                        static_cast<float>(destinationHeight) * sourceAspect));
                const int margin = (destinationWidth - fittedWidth) / 2;
                fittedX0 = dstX0 + margin;
                fittedX1 = fittedX0 + fittedWidth;
            }

            GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
            GLfloat previousClear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            glGetFloatv(GL_COLOR_CLEAR_VALUE, previousClear);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glClearColor(previousClear[0], previousClear[1],
                         previousClear[2], previousClear[3]);
            if (scissorEnabled == GL_TRUE) {
                glEnable(GL_SCISSOR_TEST);
            }

            if (!gLoggedAspectFit) {
                gLoggedAspectFit = true;
                __android_log_print(ANDROID_LOG_INFO, "reVC-XR",
                        "aspect-fit stereo presentation copy: source=%dx%d destination=%dx%d fitted=[%d,%d -> %d,%d]",
                        sourceWidth, sourceHeight,
                        destinationWidth, destinationHeight,
                        fittedX0, fittedY0, fittedX1, fittedY1);
            }

            __real_glBlitFramebuffer(
                    srcX0, srcY0, srcX1, srcY1,
                    fittedX0, fittedY0, fittedX1, fittedY1,
                    mask, filter);
            return;
        }
    }

    __real_glBlitFramebuffer(
            srcX0, srcY0, srcX1, srcY1,
            dstX0, dstY0, dstX1, dstY1,
            mask, filter);
}

#endif
