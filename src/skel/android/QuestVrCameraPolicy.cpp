#if defined(ANDROID)

#include <android/log.h>

#include <cmath>
#include <cstring>

#include "common.h"
#include "rwcore.h"
#include "main.h"
#include "Camera.h"
#include "Draw.h"
class CPtrList;
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Bones.h"
#include "RwHelper.h"
#include "RpAnimBlend.h"
#include "QuestOpenXR.h"

namespace {

const char* const kTag = "reVC-XR";

enum class VrCameraMode {
    None,
    FirstPersonOnFoot,
    ScriptedCamera,
    VehicleCamera,
    StableGameCamera,
};

struct Quaternion {
    float x;
    float y;
    float z;
    float w;
};

VrCameraMode gMode = VrCameraMode::None;
bool gOrientationOriginValid = false;
Quaternion gOrientationOrigin = {0.0f, 0.0f, 0.0f, 1.0f};

bool gOriginalCameraSaved = false;
CMatrix gOriginalCameraMatrix;
RwV2d gOriginalViewWindow;
RwV2d gOriginalViewOffset;
float gOriginalFov = 70.0f;
float gOriginalNearClip = DEFAULT_NEAR;

bool gLastStableCameraValid = false;
CMatrix gLastStableCamera;
bool gLoggedIpdOnly = false;
bool gLoggedFirstPerson = false;

float Dot(const CVector& a, const CVector& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Quaternion Normalize(const Quaternion& value) {
    const float length = sqrtf(value.x * value.x + value.y * value.y +
                               value.z * value.z + value.w * value.w);
    if (length <= 0.00001f) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    return {value.x / length, value.y / length,
            value.z / length, value.w / length};
}

Quaternion Conjugate(const Quaternion& value) {
    return {-value.x, -value.y, -value.z, value.w};
}

Quaternion Multiply(const Quaternion& a, const Quaternion& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
    };
}

Quaternion EyeOrientation(const QuestOpenXR::EyeView& eye) {
    return Normalize({eye.orientation[0], eye.orientation[1],
                      eye.orientation[2], eye.orientation[3]});
}

void SetEyeOrientation(QuestOpenXR::EyeView* eye,
                       const Quaternion& orientation) {
    const Quaternion normal = Normalize(orientation);
    eye->orientation[0] = normal.x;
    eye->orientation[1] = normal.y;
    eye->orientation[2] = normal.z;
    eye->orientation[3] = normal.w;
}

const char* ModeName(VrCameraMode mode) {
    switch (mode) {
    case VrCameraMode::FirstPersonOnFoot:
        return "first-person on-foot";
    case VrCameraMode::ScriptedCamera:
        return "scripted/cutscene";
    case VrCameraMode::VehicleCamera:
        return "vehicle camera";
    case VrCameraMode::StableGameCamera:
        return "stable game camera";
    default:
        return "none";
    }
}

VrCameraMode DetermineMode() {
    CPlayerPed* player = FindPlayerPed();
    const int cameraMode = TheCamera.Cams[TheCamera.ActiveCam].Mode;
    const bool scripted =
            TheCamera.WhoIsInControlOfTheCamera != CAMCONTROL_GAME ||
            cameraMode == CCam::MODE_FLYBY ||
            TheCamera.m_bStartingSpline;

    if (scripted) {
        return VrCameraMode::ScriptedCamera;
    }
    if (player != nil && player->bInVehicle) {
        return VrCameraMode::VehicleCamera;
    }
    if (player != nil && player->m_rwObject != nil) {
        return VrCameraMode::FirstPersonOnFoot;
    }
    return VrCameraMode::StableGameCamera;
}

bool IsLargeCameraCut(const CMatrix& current) {
    if (!gLastStableCameraValid) {
        return true;
    }

    const CVector positionDelta =
            current.GetPosition() - gLastStableCamera.GetPosition();
    CVector oldForward = gLastStableCamera.GetForward();
    CVector newForward = current.GetForward();
    oldForward.Normalise();
    newForward.Normalise();
    return positionDelta.MagnitudeSqr() > 9.0f ||
           Dot(oldForward, newForward) < 0.65f;
}

void RecenterOrientation(const QuestOpenXR::EyeView* eyes,
                         uint32_t eyeCount, const char* reason) {
    if (eyes == NULL || eyeCount == 0 || !eyes[0].valid) {
        return;
    }
    gOrientationOrigin = EyeOrientation(eyes[0]);
    gOrientationOriginValid = true;
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "VR head orientation recentered for %s", reason);
}

void RemoveHeadCentreTranslation(QuestOpenXR::EyeView* eyes,
                                 uint32_t eyeCount) {
    if (eyes == NULL || eyeCount == 0) {
        return;
    }

    float centre[3] = {0.0f, 0.0f, 0.0f};
    uint32_t validCount = 0;
    for (uint32_t eye = 0; eye < eyeCount; ++eye) {
        if (!eyes[eye].valid) {
            continue;
        }
        centre[0] += eyes[eye].position[0];
        centre[1] += eyes[eye].position[1];
        centre[2] += eyes[eye].position[2];
        ++validCount;
    }
    if (validCount == 0) {
        return;
    }

    const float inverse = 1.0f / static_cast<float>(validCount);
    centre[0] *= inverse;
    centre[1] *= inverse;
    centre[2] *= inverse;
    for (uint32_t eye = 0; eye < eyeCount; ++eye) {
        if (!eyes[eye].valid) {
            continue;
        }
        eyes[eye].position[0] -= centre[0];
        eyes[eye].position[1] -= centre[1];
        eyes[eye].position[2] -= centre[2];
    }

    if (!gLoggedIpdOnly) {
        gLoggedIpdOnly = true;
        __android_log_write(ANDROID_LOG_INFO, kTag,
                "VR camera uses rotation plus per-eye IPD only; free head-centre translation is disabled");
    }
}

void ApplyRecenteredOrientation(QuestOpenXR::EyeView* eyes,
                                uint32_t eyeCount) {
    if (!gOrientationOriginValid) {
        RecenterOrientation(eyes, eyeCount, "initial camera mode");
    }
    const Quaternion inverseOrigin = Conjugate(gOrientationOrigin);
    for (uint32_t eye = 0; eye < eyeCount; ++eye) {
        if (!eyes[eye].valid) {
            continue;
        }
        SetEyeOrientation(&eyes[eye], Multiply(
                inverseOrigin, EyeOrientation(eyes[eye])));
    }
}

void PushCameraToRenderWare() {
    if (Scene.camera == nil) {
        return;
    }
    RwFrame* frame = RwCameraGetFrame(Scene.camera);
    if (frame == nil) {
        return;
    }
    RwMatrix* matrix = RwFrameGetMatrix(frame);
    *RwMatrixGetPos(matrix) = TheCamera.GetPosition();
    *RwMatrixGetAt(matrix) = TheCamera.GetForward();
    *RwMatrixGetUp(matrix) = TheCamera.GetUp();
    *RwMatrixGetRight(matrix) = TheCamera.GetRight();
    RwMatrixUpdate(matrix);
    RwFrameUpdateObjects(frame);
    RwFrameOrthoNormalize(frame);
    TheCamera.m_vecGameCamPos = TheCamera.GetPosition();
}

void SaveOriginalGameCamera() {
    if (gOriginalCameraSaved || Scene.camera == nil) {
        return;
    }
    gOriginalCameraMatrix = TheCamera.GetMatrix();
    gOriginalFov = CDraw::GetFOV();
    gOriginalViewWindow = *RwCameraGetViewWindow(Scene.camera);
    gOriginalViewOffset = *RwCameraGetViewOffset(Scene.camera);
    gOriginalNearClip = RwCameraGetNearClipPlane(Scene.camera);
    gOriginalCameraSaved = true;
}

void RestoreOriginalGameCamera() {
    if (!gOriginalCameraSaved || Scene.camera == nil) {
        return;
    }
    TheCamera.GetMatrix() = gOriginalCameraMatrix;
    CDraw::SetFOV(gOriginalFov);
    TheCamera.CalculateDerivedValues();
    PushCameraToRenderWare();
    RwCameraSetViewWindow(Scene.camera, &gOriginalViewWindow);
    RwCameraSetViewOffset(Scene.camera, &gOriginalViewOffset);
    RwCameraSetNearClipPlane(Scene.camera, gOriginalNearClip);
    gOriginalCameraSaved = false;
}

bool BuildPlayerHeadCamera() {
    CPlayerPed* player = FindPlayerPed();
    if (player == nil || player->m_rwObject == nil ||
        player->m_pFrames[PED_HEAD] == nil) {
        return false;
    }

    CVector forward = player->GetForward();
    forward.z = 0.0f;
    if (forward.MagnitudeSqr() < 0.0001f) {
        forward = gOriginalCameraMatrix.GetForward();
        forward.z = 0.0f;
    }
    forward.Normalise();

    CVector up(0.0f, 0.0f, 1.0f);
    CVector physicalRight = CrossProduct(forward, up);
    physicalRight.Normalise();
    up = CrossProduct(physicalRight, forward);
    up.Normalise();

    CVector headPosition = player->GetNodePosition(PED_HEAD);
    headPosition += forward * 0.08f;
    headPosition += up * 0.02f;

#ifdef PED_SKIN
    RpClump* clump = player->GetClump();
    if (clump != nil && IsClumpSkinned(clump)) {
        RpHAnimHierarchy* hierarchy = GetAnimHierarchyFromSkinClump(clump);
        if (hierarchy != nil) {
            const int32 index = RpHAnimIDGetIndex(
                    hierarchy, ConvertPedNode2BoneTag(PED_HEAD));
            if (index >= 0) {
                RwMatrix* matrices = RpHAnimHierarchyGetMatrixArray(hierarchy);
                RwV3d hiddenScale = {0.0f, 0.0f, 0.0f};
                RwMatrixScale(&matrices[index], &hiddenScale,
                              rwCOMBINEPRECONCAT);
            }
        }
    }
#endif

    CMatrix firstPerson = gOriginalCameraMatrix;
    firstPerson.GetPosition() = headPosition;
    firstPerson.GetForward() = forward;
    firstPerson.GetRight() = CrossProduct(up, forward);
    firstPerson.GetUp() = up;
    TheCamera.GetMatrix() = firstPerson;
    TheCamera.CalculateDerivedValues();
    PushCameraToRenderWare();
    RwCameraSetNearClipPlane(Scene.camera, 0.05f);

    if (!gLoggedFirstPerson) {
        gLoggedFirstPerson = true;
        __android_log_print(ANDROID_LOG_INFO, kTag,
                "VR on-foot camera anchored to player head at %.3f %.3f %.3f; head mesh hidden and near clip=0.05",
                headPosition.x, headPosition.y, headPosition.z);
    }
    return true;
}

void PrepareVrCamera(QuestOpenXR::EyeView* eyes, uint32_t eyeCount) {
    const VrCameraMode newMode = DetermineMode();
    const bool modeChanged = newMode != gMode;
    const CMatrix currentGameCamera = TheCamera.GetMatrix();
    const bool cameraCut = newMode == VrCameraMode::ScriptedCamera &&
                           IsLargeCameraCut(currentGameCamera);

    if (modeChanged || cameraCut) {
        RecenterOrientation(eyes, eyeCount,
                modeChanged ? ModeName(newMode) : "cutscene camera cut");
    }

    if (modeChanged) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "VR camera mode -> %s", ModeName(newMode));
        gMode = newMode;
    }

    RemoveHeadCentreTranslation(eyes, eyeCount);
    ApplyRecenteredOrientation(eyes, eyeCount);

    SaveOriginalGameCamera();
    if (newMode == VrCameraMode::FirstPersonOnFoot) {
        if (!BuildPlayerHeadCamera()) {
            __android_log_write(ANDROID_LOG_WARN, kTag,
                    "player head anchor unavailable; retaining stable game camera for this frame");
        }
    }

    if (newMode == VrCameraMode::ScriptedCamera) {
        gLastStableCamera = currentGameCamera;
        gLastStableCameraValid = true;
    } else {
        gLastStableCameraValid = false;
    }
}

} // namespace

extern "C" QuestOpenXR::StereoFrameResult
__real__ZN11QuestOpenXR16BeginStereoFrameEPNS_7EyeViewEjPj(
        QuestOpenXR::EyeView* eyes, uint32_t eyeCapacity,
        uint32_t* eyeCount);

extern "C" QuestOpenXR::StereoFrameResult
__wrap__ZN11QuestOpenXR16BeginStereoFrameEPNS_7EyeViewEjPj(
        QuestOpenXR::EyeView* eyes, uint32_t eyeCapacity,
        uint32_t* eyeCount) {
    const QuestOpenXR::StereoFrameResult result =
            __real__ZN11QuestOpenXR16BeginStereoFrameEPNS_7EyeViewEjPj(
                    eyes, eyeCapacity, eyeCount);
    if (result == QuestOpenXR::StereoFrameResult::Render &&
        eyes != NULL && eyeCount != NULL && *eyeCount > 0) {
        PrepareVrCamera(eyes, *eyeCount);
    }
    return result;
}

extern "C" QuestOpenXR::FrameResult
__real__ZN11QuestOpenXR14EndStereoFrameEv();

extern "C" QuestOpenXR::FrameResult
__wrap__ZN11QuestOpenXR14EndStereoFrameEv() {
    const QuestOpenXR::FrameResult result =
            __real__ZN11QuestOpenXR14EndStereoFrameEv();
    RestoreOriginalGameCamera();
    return result;
}

#endif
