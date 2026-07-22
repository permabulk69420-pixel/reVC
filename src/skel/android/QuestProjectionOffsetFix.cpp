#if defined(ANDROID)

#include <android/log.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <sys/stat.h>

#include "common.h"
#include "rwcore.h"
#include "QuestOpenXR.h"

namespace {

bool gLoggedProjectionCorrection = false;

void LogProjectionCorrection(const RwV2d& input, const RwV2d& corrected) {
    char text[256];
    snprintf(text, sizeof(text),
             "corrected RenderWare stereo viewOffset sign: input=(%.6f,%.6f) applied=(%.6f,%.6f)",
             input.x, input.y, corrected.x, corrected.y);
    __android_log_write(ANDROID_LOG_INFO, "reVC-XR", text);

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
        fprintf(file, "HANDOFF %s\n", text);
        fclose(file);
    }
}

} // namespace

extern "C" RwCamera* __real_RwCameraSetViewOffset(
        RwCamera* camera, const RwV2d* offset);

extern "C" RwCamera* __wrap_RwCameraSetViewOffset(
        RwCamera* camera, const RwV2d* offset) {
    if (offset == NULL || !QuestOpenXR::OwnsPresentation()) {
        return __real_RwCameraSetViewOffset(camera, offset);
    }

    // librw's perspective frustum uses the opposite viewOffset sign from the
    // OpenXR tangent-centre convention. The previous bridge passed the OpenXR
    // centre directly, shifting each eye farther in the wrong direction and
    // producing the strongly angled/zoomed presentation seen on Quest hardware.
    const RwV2d corrected = {-offset->x, -offset->y};
    if (!gLoggedProjectionCorrection &&
        (fabsf(offset->x) > 0.000001f || fabsf(offset->y) > 0.000001f)) {
        gLoggedProjectionCorrection = true;
        LogProjectionCorrection(*offset, corrected);
    }
    return __real_RwCameraSetViewOffset(camera, &corrected);
}

#endif
