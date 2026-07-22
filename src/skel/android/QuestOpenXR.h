#pragma once

#if defined(ANDROID)

#include <stdint.h>

// reVC's common header defines debug(...) as a macro. The OpenXR bridge uses
// ordinary descriptive member names and does not use that logging macro.
#ifdef debug
#undef debug
#endif

// RenderWare state reset used when replaying the captured pass for eye 1.
void DefinedState(void);

namespace QuestOpenXR {

enum class FrameResult {
    Inactive,
    WaitingForSession,
    Presented,
    ExitRequested,
    FatalError,
};

enum class StereoFrameResult {
    Inactive,
    WaitingForSession,
    Render,
    SkipRender,
    ExitRequested,
    FatalError,
};

struct HeadState {
    bool valid;
    float orientation[4]; // x, y, z, w relative to the launch orientation
    float tanHalfFov[2];  // horizontal and vertical
};

struct EyeView {
    bool valid;
    float orientation[4]; // x, y, z, w relative to the launch orientation
    float position[3];    // metres in the launch-head coordinate frame
    float angleLeft;
    float angleRight;
    float angleUp;
    float angleDown;
    int width;
    int height;
};

// Every function is called by SDL's existing render thread with SDL's GLES
// context current. No native side thread and no second EGL context exist.
bool Initialize();
bool AwaitSessionReady(uint32_t timeoutMilliseconds);

// Legacy mono submission is retained for the one flat bootstrap frame that
// creates and validates the OpenXR runtime. Normal immersive frames use the
// split begin/eye/end API below.
FrameResult SubmitGameFrame(unsigned int sourceFramebuffer,
                            int sourceWidth, int sourceHeight);

StereoFrameResult BeginStereoFrame(EyeView* eyes, uint32_t eyeCapacity,
                                   uint32_t* eyeCount);
bool SubmitStereoEye(uint32_t eye, unsigned int sourceFramebuffer,
                     int sourceWidth, int sourceHeight);
FrameResult EndStereoFrame();
bool StereoFrameActive();

void PollEvents();
void Shutdown();
void FinalizeAfterEngineShutdown();

int RecommendedWidth();
int RecommendedHeight();
bool GetHeadState(HeadState* state);
bool OwnsPresentation();
bool ExitRequested();

} // namespace QuestOpenXR

#endif
