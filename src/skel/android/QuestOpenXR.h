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

// Every function is called by the game's existing native/render thread. That
// thread creates the sole EGL context before RenderWare adopts it; no Android
// render surface, native side thread, or second EGL context exists.
bool Initialize();
bool StartSession();
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
void RequestExitFromActivity();

} // namespace QuestOpenXR

#endif
