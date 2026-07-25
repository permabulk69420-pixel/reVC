#pragma once

// Head pose for the VR first-person camera, published for core/Cam.cpp.
//
// CCam::Process_1stPerson already places the camera at the player's head bone
// and derives its direction from Beta (yaw) and Alpha (pitch). Driving those two
// angles from the headset instead of the look stick turns it into a real VR
// camera while leaving the collision checks, near-plane handling and every other
// downstream consumer of Front working exactly as before.
//
// Doing it this way also removes the reason the view shears when you turn your
// head. The compositor reprojects using the difference between the pose a frame
// declared and where the head is at display time. While the game camera supplies
// its own changing rotation, that difference is polluted and straight edges
// warp. Once the camera is a constant recentre offset composed with the head
// orientation, the constant cancels out of the difference and the submitted pose
// is correct by construction.
//
// The pose is one frame old: the runtime pose for a frame is fetched during
// ConstructRenderList, which runs after the camera has been processed. That
// latency is what asynchronous timewarp exists to absorb.

#include "common.h"

namespace QuestVrCamera {

// True once a stereo frame has supplied a tracked head pose, and the VR
// first-person camera should drive the game camera.
bool IsHeadTrackingActive(void);

// True once RecentreToPlayerHeading has established the yaw reference. Until
// then GetHeadAngles refuses, so callers must keep retrying the recentre.
bool IsRecentred(void);

// Head yaw and pitch in GTA's convention: yaw measured in the XY plane, pitch
// positive upward, both radians, already mapped out of OpenXR's axes and
// through the recentre offset. Returns false when no pose is available, in
// which case the caller must leave its own angles alone.
bool GetHeadAngles(float* yaw, float* pitch);

// Head translation relative to the recentre origin, in game units, expressed in
// GTA world axes. Small by construction: this is head movement within a seated
// or standing volume, not locomotion.
bool GetHeadPositionOffset(CVector* offset);

// Captures the current head orientation as the forward reference, so that the
// player's in-game facing and the direction the user is physically looking are
// aligned from that moment on.
void RecentreToPlayerHeading(float playerHeadingRadians);

} // namespace QuestVrCamera
