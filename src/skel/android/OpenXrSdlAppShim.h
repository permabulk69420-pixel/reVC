#pragma once

// reVC retains SDL for events but has no real SDL_Window on Quest. Keep
// legacy menu cursor-centering calls from touching librw's opaque HMD token.

#if defined(REVC_OPENXR_DIRECT_CONTEXT) && defined(LIBRW_SDL2)

#include <SDL.h>

static inline void RevcSdlWarpMouseInWindow(SDL_Window *, int, int)
{
}

#define SDL_WarpMouseInWindow RevcSdlWarpMouseInWindow

#endif
