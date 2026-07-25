#if defined(ANDROID)

// Keep the proven loader/session/swapchain implementation byte-for-byte in one
// translation unit, then extend it with a frame lifecycle that lets reVC render
// each eye before OpenXR submits the projection layer.
#define SubmitGameFrame SubmitGameFrameLegacy
#include "QuestOpenXR.cpp"
#undef SubmitGameFrame

namespace {

struct StereoRuntimeState {
    bool active;
    bool shouldRender;
    bool originSet;
    bool loggedFirstFrame;
    XrTime displayTime;
    uint32_t viewCount;
    uint32_t submittedMask;
    XrQuaternionf originOrientation;
    XrVector3f originPosition;

    StereoRuntimeState()
        : active(false), shouldRender(false), originSet(false),
          loggedFirstFrame(false), displayTime(0), viewCount(0),
          submittedMask(0) {
        originOrientation = {0.0f, 0.0f, 0.0f, 1.0f};
        originPosition = {0.0f, 0.0f, 0.0f};
    }
};

StereoRuntimeState stereo;

XrVector3f RotateVector(const XrQuaternionf& rotation,
                        const XrVector3f& value) {
    const XrQuaternionf vector = {value.x, value.y, value.z, 0.0f};
    const XrQuaternionf rotated = Multiply(
            Multiply(rotation, vector), Conjugate(rotation));
    return {rotated.x, rotated.y, rotated.z};
}

void ResetStereoFrame() {
    stereo.active = false;
    stereo.shouldRender = false;
    stereo.displayTime = 0;
    stereo.viewCount = 0;
    stereo.submittedMask = 0;
}

bool BlitRenderedEye(const Swapchain& swapchain, uint32_t eye,
                     GLuint sourceFramebuffer, int sourceWidth,
                     int sourceHeight) {
    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquire = {
        XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO
    };
    if (!Check(xrAcquireSwapchainImage(
            swapchain.handle, &acquire, &imageIndex),
            "xrAcquireSwapchainImage(stereo)")) {
        return false;
    }

    XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (!Check(xrWaitSwapchainImage(swapchain.handle, &wait),
               "xrWaitSwapchainImage(stereo)")) {
        XrSwapchainImageReleaseInfo release = {
            XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO
        };
        xrReleaseSwapchainImage(swapchain.handle, &release);
        return false;
    }

    SavedGlState saved;
    SaveGlState(&saved);
    while (glGetError() != GL_NO_ERROR) {
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
        glBlitFramebuffer(0, 0, sourceWidth, sourceHeight,
                          0, 0, swapchain.width, swapchain.height,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glFlush();
        const GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            Log("Stereo eye %u blit failed: GL=0x%x", eye, error);
            valid = false;
        }
    } else {
        Log("Stereo eye %u framebuffer incomplete: read=0x%x draw=0x%x",
            eye, glCheckFramebufferStatus(GL_READ_FRAMEBUFFER),
            glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER));
    }

    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, 0, 0);
    RestoreGlState(saved);

    XrSwapchainImageReleaseInfo release = {
        XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO
    };
    if (!Check(xrReleaseSwapchainImage(swapchain.handle, &release),
               "xrReleaseSwapchainImage(stereo)")) {
        valid = false;
    }
    return valid;
}

QuestOpenXR::FrameResult EndStereoFrameInternal(bool fatalOnMissingEyes) {
    if (!stereo.active) {
        return QuestOpenXR::FrameResult::Inactive;
    }

    XrCompositionLayerProjection layer = {
        XR_TYPE_COMPOSITION_LAYER_PROJECTION
    };
    const XrCompositionLayerBaseHeader* layers[1] = {NULL};
    uint32_t layerCount = 0;

    const uint32_t expectedMask = stereo.viewCount >= 32
            ? 0xFFFFFFFFu : ((1u << stereo.viewCount) - 1u);
    const bool complete = !stereo.shouldRender ||
            stereo.submittedMask == expectedMask;

    if (stereo.shouldRender && complete) {
        for (uint32_t eye = 0; eye < stereo.viewCount; ++eye) {
            XrCompositionLayerProjectionView& projection =
                    g.projectionViews[eye];
            projection = XrCompositionLayerProjectionView{
                    XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            projection.pose = g.views[eye].pose;
            projection.fov = g.views[eye].fov;

            // The submitted pose tells the compositor which camera produced
            // this image, and it reprojects from that pose to display time.
            // reVC does not render from this pose: ApplyEyeCamera applies the
            // XR rotation relative to the game camera basis instead. If those
            // disagree, head rotation is effectively applied twice. Report the
            // pose so it can be compared against the rendered basis already in
            // the log.
            static unsigned int loggedPoses = 0;
            if (loggedPoses < 8) {
                ++loggedPoses;
                Log("submitted eye%u pose q=%.3f,%.3f,%.3f,%.3f p=%.2f,%.2f,%.2f",
                    eye, g.views[eye].pose.orientation.x,
                    g.views[eye].pose.orientation.y,
                    g.views[eye].pose.orientation.z,
                    g.views[eye].pose.orientation.w,
                    g.views[eye].pose.position.x,
                    g.views[eye].pose.position.y,
                    g.views[eye].pose.position.z);
            }
            projection.subImage.swapchain = g.swapchains[eye].handle;
            projection.subImage.imageRect.offset = {0, 0};
            projection.subImage.imageRect.extent = {
                g.swapchains[eye].width, g.swapchains[eye].height
            };
        }
        layer.space = g.appSpace;
        layer.viewCount = stereo.viewCount;
        layer.views = g.projectionViews.data();
        layers[0] = reinterpret_cast<
                const XrCompositionLayerBaseHeader*>(&layer);
        layerCount = 1;
    } else if (stereo.shouldRender && fatalOnMissingEyes) {
        Log("Stereo frame missing eye submissions: mask=0x%x expected=0x%x",
            stereo.submittedMask, expectedMask);
        g.fatalError = true;
        g.exitRequested.store(true);
    }

    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = stereo.displayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layerCount == 0 ? NULL : layers;
    const bool ended = Check(xrEndFrame(g.session, &endInfo),
                             "xrEndFrame(stereo)");

    ResetStereoFrame();
    if (!ended) {
        g.fatalError = true;
        g.exitRequested.store(true);
        return QuestOpenXR::FrameResult::FatalError;
    }

    ++g.frameCount;
    if (!stereo.loggedFirstFrame) {
        stereo.loggedFirstFrame = true;
        Log("True per-eye render submission active; finished flat-frame copying");
    }
    if (g.frameCount == 1 || g.frameCount % 300 == 0) {
        Log("reVC stereo frames submitted=%llu state=%s",
            static_cast<unsigned long long>(g.frameCount),
            SessionStateName(g.sessionState));
    }

    if (g.exitRequested.load()) {
        return QuestOpenXR::FrameResult::ExitRequested;
    }
    return g.fatalError ? QuestOpenXR::FrameResult::FatalError
                        : QuestOpenXR::FrameResult::Presented;
}

} // namespace

namespace QuestOpenXR {

FrameResult SubmitGameFrame(unsigned int sourceFramebuffer,
                            int sourceWidth, int sourceHeight) {
    return SubmitGameFrameLegacy(sourceFramebuffer, sourceWidth, sourceHeight);
}

StereoFrameResult BeginStereoFrame(EyeView* eyes, uint32_t eyeCapacity,
                                   uint32_t* eyeCount) {
    if (eyeCount != NULL) {
        *eyeCount = 0;
    }
    if (!g.initialized) {
        return StereoFrameResult::Inactive;
    }
    if (!OnOwnerThread("BeginStereoFrame")) {
        return StereoFrameResult::FatalError;
    }
    if (stereo.active) {
        Log("BeginStereoFrame called while another frame is active");
        g.fatalError = true;
        g.exitRequested.store(true);
        return StereoFrameResult::FatalError;
    }
    if (!PollEventsInternal() || g.fatalError) {
        return StereoFrameResult::FatalError;
    }
    if (g.exitRequested.load()) {
        return StereoFrameResult::ExitRequested;
    }
    if (!g.sessionRunning) {
        // "waiting for session" on its own does not say why. sessionRunning is
        // only cleared by an xrEndSession after STOPPING, or by a full runtime
        // teardown, and hardware logs show it going false with neither of those
        // logged. Report the session state so the next capture names the cause,
        // and report it again whenever that state changes rather than per frame.
        static XrSessionState loggedWaitState = XR_SESSION_STATE_UNKNOWN;
        static bool loggedWaitOnce = false;
        if (!loggedWaitOnce || loggedWaitState != g.sessionState) {
            loggedWaitOnce = true;
            loggedWaitState = g.sessionState;
            Log("stereo waiting: sessionRunning=0 state=%s exitRequested=%d fatal=%d frames=%llu",
                SessionStateName(g.sessionState),
                g.exitRequested.load() ? 1 : 0, g.fatalError ? 1 : 0,
                static_cast<unsigned long long>(g.frameCount));
        }
        return StereoFrameResult::WaitingForSession;
    }

    XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState = {XR_TYPE_FRAME_STATE};
    if (!Check(xrWaitFrame(g.session, &waitInfo, &frameState),
               "xrWaitFrame(stereo)")) {
        g.fatalError = true;
        g.exitRequested.store(true);
        return StereoFrameResult::FatalError;
    }

    XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
    if (!Check(xrBeginFrame(g.session, &beginInfo),
               "xrBeginFrame(stereo)")) {
        g.fatalError = true;
        g.exitRequested.store(true);
        return StereoFrameResult::FatalError;
    }

    stereo.active = true;
    stereo.shouldRender = frameState.shouldRender == XR_TRUE;
    stereo.displayTime = frameState.predictedDisplayTime;
    stereo.viewCount = 0;
    stereo.submittedMask = 0;

    if (!stereo.shouldRender) {
        return StereoFrameResult::SkipRender;
    }

    XrViewLocateInfo locate = {XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = kViewType;
    locate.displayTime = frameState.predictedDisplayTime;
    locate.space = g.appSpace;
    XrViewState viewState = {XR_TYPE_VIEW_STATE};
    uint32_t locatedCount = 0;
    const XrResult locateResult = xrLocateViews(
            g.session, &locate, &viewState,
            static_cast<uint32_t>(g.views.size()), &locatedCount,
            g.views.data());
    const XrViewStateFlags required =
            XR_VIEW_STATE_ORIENTATION_VALID_BIT |
            XR_VIEW_STATE_POSITION_VALID_BIT;
    if (XR_FAILED(locateResult) ||
        (viewState.viewStateFlags & required) != required ||
        locatedCount == 0 || locatedCount != g.swapchains.size()) {
        if (XR_FAILED(locateResult)) {
            Log("xrLocateViews(stereo) failed: %s", ResultName(locateResult));
        } else {
            Log("Stereo views not ready: flags=0x%llx count=%u",
                static_cast<unsigned long long>(viewState.viewStateFlags),
                locatedCount);
        }
        stereo.shouldRender = false;
        return StereoFrameResult::SkipRender;
    }
    if (eyes == NULL || eyeCapacity < locatedCount) {
        Log("Stereo eye output capacity=%u is smaller than runtime count=%u",
            eyeCapacity, locatedCount);
        EndStereoFrameInternal(false);
        g.fatalError = true;
        g.exitRequested.store(true);
        return StereoFrameResult::FatalError;
    }

    stereo.viewCount = locatedCount;
    UpdateHeadState(locatedCount);

    if (!stereo.originSet) {
        // Recentre yaw only. Pitch and roll define which way is up, and the
        // runtime's reference space is already gravity aligned, so they must
        // pass through untouched. Capturing the whole orientation makes
        // whatever angle the head happened to be at on the first tracked frame
        // become level for the rest of the session: the headset is usually
        // still being put on or adjusted at that moment, which is why the view
        // started pitched away from the horizon on every launch.
        const XrQuaternionf raw = Normalize(g.views[0].pose.orientation);
        // Forward is -Z in OpenXR, and yaw is rotation about +Y.
        const float fx = 2.0f * (raw.x * raw.z + raw.w * raw.y);
        const float fz = 1.0f - 2.0f * (raw.x * raw.x + raw.y * raw.y);
        const float yaw = atan2f(fx, fz);
        const float halfYaw = 0.5f * yaw;
        stereo.originOrientation.x = 0.0f;
        stereo.originOrientation.y = sinf(halfYaw);
        stereo.originOrientation.z = 0.0f;
        stereo.originOrientation.w = cosf(halfYaw);
        Log("Stereo origin yaw=%.1f deg captured; pitch and roll left on gravity",
            yaw * 180.0f / 3.14159265f);
        XrVector3f centre = {0.0f, 0.0f, 0.0f};
        for (uint32_t eye = 0; eye < locatedCount; ++eye) {
            centre.x += g.views[eye].pose.position.x;
            centre.y += g.views[eye].pose.position.y;
            centre.z += g.views[eye].pose.position.z;
        }
        const float inverseCount = 1.0f / static_cast<float>(locatedCount);
        centre.x *= inverseCount;
        centre.y *= inverseCount;
        centre.z *= inverseCount;
        stereo.originPosition = centre;
        stereo.originSet = true;
        Log("Stereo launch pose captured from %u tracked eyes", locatedCount);
    }

    const XrQuaternionf inverseOrigin =
            Conjugate(stereo.originOrientation);
    for (uint32_t eye = 0; eye < locatedCount; ++eye) {
        EyeView& output = eyes[eye];
        const XrQuaternionf relativeOrientation = Normalize(Multiply(
                inverseOrigin, Normalize(g.views[eye].pose.orientation)));
        const XrVector3f delta = {
            g.views[eye].pose.position.x - stereo.originPosition.x,
            g.views[eye].pose.position.y - stereo.originPosition.y,
            g.views[eye].pose.position.z - stereo.originPosition.z
        };
        const XrVector3f relativePosition = RotateVector(inverseOrigin, delta);

        output.valid = true;
        output.orientation[0] = relativeOrientation.x;
        output.orientation[1] = relativeOrientation.y;
        output.orientation[2] = relativeOrientation.z;
        output.orientation[3] = relativeOrientation.w;
        output.position[0] = relativePosition.x;
        output.position[1] = relativePosition.y;
        output.position[2] = relativePosition.z;
        output.angleLeft = g.views[eye].fov.angleLeft;
        output.angleRight = g.views[eye].fov.angleRight;
        output.angleUp = g.views[eye].fov.angleUp;
        output.angleDown = g.views[eye].fov.angleDown;
        output.width = g.swapchains[eye].width;
        output.height = g.swapchains[eye].height;
    }
    if (eyeCount != NULL) {
        *eyeCount = locatedCount;
    }
    return StereoFrameResult::Render;
}

bool SubmitStereoEye(uint32_t eye, unsigned int sourceFramebuffer,
                     int sourceWidth, int sourceHeight) {
    if (!stereo.active || !stereo.shouldRender || eye >= stereo.viewCount ||
        sourceFramebuffer == 0 || sourceWidth <= 0 || sourceHeight <= 0) {
        return false;
    }
    if (!OnOwnerThread("SubmitStereoEye")) {
        return false;
    }
    if (!BlitRenderedEye(g.swapchains[eye], eye,
                         static_cast<GLuint>(sourceFramebuffer),
                         sourceWidth, sourceHeight)) {
        g.fatalError = true;
        g.exitRequested.store(true);
        return false;
    }
    stereo.submittedMask |= (1u << eye);
    return true;
}

FrameResult EndStereoFrame() {
    if (!OnOwnerThread("EndStereoFrame")) {
        return FrameResult::FatalError;
    }
    return EndStereoFrameInternal(true);
}

bool StereoFrameActive() {
    return stereo.active;
}

} // namespace QuestOpenXR

#endif
