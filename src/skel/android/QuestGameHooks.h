#pragma once

// Frame-sequencing hooks for the Quest OpenXR stereo bridge.
//
// These were previously installed with -Wl,--wrap on the mangled names of the
// render-sequence functions in core/main.cpp. That could never work. GNU ld's
// --wrap only redirects *undefined references* between object files, and every
// one of those functions is both defined in and called from core/main.cpp, so
// the compiler resolved each call directly against the local definition and the
// linker never saw a reference to redirect. All ten wrappers were dead code:
// the pass-recording flags were never set and the right-eye replay in
// DoRWStuffEndOfFrame never ran, so the second eye was never rendered.
//
// The hooks are now ordinary calls placed inside the function bodies. Putting
// them in the body rather than at each call site means every caller is covered
// automatically, and a mistake becomes a compile error instead of silence.

#include "common.h"

namespace QuestGameHooks {

enum class FrameStart {
	Plain,
	Horizon,
};

// Called at the top of DoRWStuffStartOfFrame / _Horizon, before the frame is
// begun, so the bridge can record which start path and arguments the right-eye
// replay must reproduce.
void NoteFrameStart(FrameStart mode, int16 topRed, int16 topGreen, int16 topBlue,
		int16 bottomRed, int16 bottomGreen, int16 bottomBlue, int16 alpha);

// Called at the top of each render-sequence function so the replay knows which
// stages the left eye actually ran.
void NoteRenderScene(void);
void NoteRenderDebugShit(void);
void NoteRenderEffects(void);
void NoteRender2dStuff(void);
void NoteRenderMenus(void);
void NoteDoFade(void);
void NoteRender2dStuffAfterFade(void);

// Called at the *end* of DoRWStuffEndOfFrame, after the normal frame has been
// completed. This is where the left eye is submitted and the right eye is
// replayed, so it must run after the real body rather than before it.
void FrameEnd(void);

} // namespace QuestGameHooks
