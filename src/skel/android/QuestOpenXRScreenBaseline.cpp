#if defined(ANDROID)

#include <android/log.h>
#include <openxr/openxr.h>

namespace {

const char* const kTag = "reVC-XR";
bool gLoggedStereoScreenBaseline = false;

} // namespace

extern "C" XrResult
__real_xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo);

extern "C" XrResult
__wrap_xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) {
    if (frameEndInfo == NULL || frameEndInfo->layerCount != 1 ||
        frameEndInfo->layers == NULL || frameEndInfo->layers[0] == NULL ||
        frameEndInfo->layers[0]->type !=
                XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
        return __real_xrEndFrame(session, frameEndInfo);
    }

    const XrCompositionLayerProjection* projection =
            reinterpret_cast<const XrCompositionLayerProjection*>(
                    frameEndInfo->layers[0]);
    if (projection->views == NULL || projection->viewCount < 2) {
        return __real_xrEndFrame(session, frameEndInfo);
    }

    XrCompositionLayerQuad quads[2] = {};
    const XrCompositionLayerBaseHeader* layers[2] = {NULL, NULL};
    for (uint32_t eye = 0; eye < 2; ++eye) {
        XrCompositionLayerQuad& quad = quads[eye];
        quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        quad.layerFlags = 0;
        quad.space = projection->space;
        quad.eyeVisibility = eye == 0
                ? XR_EYE_VISIBILITY_LEFT
                : XR_EYE_VISIBILITY_RIGHT;
        quad.subImage = projection->views[eye].subImage;
        quad.pose.orientation.w = 1.0f;
        quad.pose.position.z = -2.0f;
        quad.size.width = 3.2f;
        quad.size.height = 1.8f;
        layers[eye] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                &quad);
    }

    XrFrameEndInfo rewritten = *frameEndInfo;
    rewritten.layerCount = 2;
    rewritten.layers = layers;

    if (!gLoggedStereoScreenBaseline) {
        gLoggedStereoScreenBaseline = true;
        __android_log_write(
                ANDROID_LOG_INFO, kTag,
                "zero-transform stereo-screen baseline active: eye textures submitted as 16:9 quads at 2m");
    }
    return __real_xrEndFrame(session, &rewritten);
}

#endif
