#pragma once

// Compile-time adapter for librw's SDL2 GL backend on Quest. SDL remains the
// input/event library, but its video calls are redirected to the EGL context
// already created by the sole OpenXR game thread. This lets us keep upstream
// librw as an unmodified submodule.

#if defined(REVC_OPENXR_DIRECT_CONTEXT) && defined(LIBRW_SDL2)

#include <dlfcn.h>
#include <EGL/egl.h>
#include <SDL.h>

#include <cstring>

extern "C" int RevcOpenXrRecommendedWidth(void);
extern "C" int RevcOpenXrRecommendedHeight(void);

static inline int &RevcSdlGlProfileValue()
{
    static int value;
    return value;
}

static inline int &RevcSdlGlMajorValue()
{
    static int value;
    return value;
}

static inline int &RevcSdlGlMinorValue()
{
    static int value;
    return value;
}

static inline int RevcSdlInitSubSystem(Uint32 flags)
{
    if((flags & SDL_INIT_VIDEO) != 0)
        flags = (flags & ~SDL_INIT_VIDEO) | SDL_INIT_EVENTS |
                SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER;
    return SDL_InitSubSystem(flags);
}

static inline void RevcSdlQuitSubSystem(Uint32 flags)
{
    if((flags & SDL_INIT_VIDEO) != 0)
        flags = (flags & ~SDL_INIT_VIDEO) | SDL_INIT_EVENTS |
                SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER;
    SDL_QuitSubSystem(flags);
}

static inline int RevcSdlGetNumVideoDisplays(void)
{
    return 1;
}

static inline int RevcSdlGetNumDisplayModes(int)
{
    // librw keeps mode 0 as windowed and adds this identical mode as its one
    // exclusive HMD mode. reVC then selects the latter normally.
    return 1;
}

static inline int RevcSdlFillDisplayMode(SDL_DisplayMode *mode)
{
    if(mode == NULL)
        return -1;
    std::memset(mode, 0, sizeof(*mode));
    mode->format = SDL_PIXELFORMAT_RGBA8888;
    mode->w = RevcOpenXrRecommendedWidth();
    mode->h = RevcOpenXrRecommendedHeight();
    mode->refresh_rate = 0;
    return mode->w > 0 && mode->h > 0 ? 0 : -1;
}

static inline int RevcSdlGetCurrentDisplayMode(int, SDL_DisplayMode *mode)
{
    return RevcSdlFillDisplayMode(mode);
}

static inline int RevcSdlGetDisplayMode(int, int, SDL_DisplayMode *mode)
{
    return RevcSdlFillDisplayMode(mode);
}

static inline const char *RevcSdlGetDisplayName(int display)
{
    return display == 0 ? "OpenXR HMD" : NULL;
}

static inline int RevcSdlGlSetAttribute(SDL_GLattr attribute, int value)
{
    if(attribute == SDL_GL_CONTEXT_PROFILE_MASK)
        RevcSdlGlProfileValue() = value;
    else if(attribute == SDL_GL_CONTEXT_MAJOR_VERSION)
        RevcSdlGlMajorValue() = value;
    else if(attribute == SDL_GL_CONTEXT_MINOR_VERSION)
        RevcSdlGlMinorValue() = value;
    return 0;
}

static inline SDL_Window *RevcSdlCreateWindow(
        const char *, int, int, int, int, Uint32)
{
    // librw tries desktop profiles first. Only allow its GLES 3.1 attempt to
    // adopt the context; no SDL/Android window is actually created.
    if(RevcSdlGlProfileValue() != SDL_GL_CONTEXT_PROFILE_ES ||
       RevcSdlGlMajorValue() < 3 ||
       (RevcSdlGlMajorValue() == 3 && RevcSdlGlMinorValue() < 1))
        return NULL;
    static unsigned char windowToken;
    return reinterpret_cast<SDL_Window*>(&windowToken);
}

static inline int RevcSdlSetWindowDisplayMode(
        SDL_Window *, const SDL_DisplayMode *)
{
    return 0;
}

static inline SDL_GLContext RevcSdlGlCreateContext(SDL_Window *)
{
    return reinterpret_cast<SDL_GLContext>(eglGetCurrentContext());
}

static inline void *RevcSdlGlGetProcAddress(const char *name)
{
    void *proc = dlsym(RTLD_DEFAULT, name);
    if(proc == NULL) {
        const __eglMustCastToProperFunctionPointerType eglProc =
                eglGetProcAddress(name);
        static_assert(sizeof(proc) == sizeof(eglProc),
                      "EGL function pointers must fit GLAD's loader type");
        std::memcpy(&proc, &eglProc, sizeof(proc));
    }
    return proc;
}

static inline void RevcSdlGetWindowSize(SDL_Window *, int *width, int *height)
{
    if(width != NULL)
        *width = RevcOpenXrRecommendedWidth();
    if(height != NULL)
        *height = RevcOpenXrRecommendedHeight();
}

static inline int RevcSdlGlSetSwapInterval(int)
{
    return 0;
}

static inline void RevcSdlGlSwapWindow(SDL_Window *)
{
}

static inline void RevcSdlGlDeleteContext(SDL_GLContext)
{
}

static inline void RevcSdlDestroyWindow(SDL_Window *)
{
}

#define SDL_InitSubSystem RevcSdlInitSubSystem
#define SDL_QuitSubSystem RevcSdlQuitSubSystem
#define SDL_GetNumVideoDisplays RevcSdlGetNumVideoDisplays
#define SDL_GetNumDisplayModes RevcSdlGetNumDisplayModes
#define SDL_GetCurrentDisplayMode RevcSdlGetCurrentDisplayMode
#define SDL_GetDisplayMode RevcSdlGetDisplayMode
#define SDL_GetDisplayName RevcSdlGetDisplayName
#define SDL_GL_SetAttribute RevcSdlGlSetAttribute
#define SDL_CreateWindow RevcSdlCreateWindow
#define SDL_SetWindowDisplayMode RevcSdlSetWindowDisplayMode
#define SDL_GL_CreateContext RevcSdlGlCreateContext
#define SDL_GL_GetProcAddress RevcSdlGlGetProcAddress
#define SDL_GetWindowSize RevcSdlGetWindowSize
#define SDL_GL_SetSwapInterval RevcSdlGlSetSwapInterval
#define SDL_GL_SwapWindow RevcSdlGlSwapWindow
#define SDL_GL_DeleteContext RevcSdlGlDeleteContext
#define SDL_DestroyWindow RevcSdlDestroyWindow

#endif
