#pragma once

#if defined(ANDROID)

#include <stdint.h>

namespace QuestOpenXR {

enum class FrameResult {
    Inactive,
    WaitingForSession,
    Presented,
    ExitRequested,
    FatalError,
};

struct HeadState {
    bool valid;
    float orientation[4]; // x, y, z, w relative to the launch orientation
    float tanHalfFov[2];  // horizontal and vertical
};

// Every function is called by SDL's existing render thread with SDL's GLES
// context current. No native side thread and no second EGL context exist.
bool Initialize();
bool AwaitSessionReady(uint32_t timeoutMilliseconds);
FrameResult SubmitGameFrame(unsigned int sourceFramebuffer,
                            int sourceWidth, int sourceHeight);
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
