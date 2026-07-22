#if defined(ANDROID)

#include "common.h"

// QuestOpenXRIntegration.cpp records the original start-of-frame parameters
// inside an anonymous namespace. Export exact aliases for those two helper
// declarations without changing the proven RenderWare functions themselves.
extern "C" bool RealHorizonTarget(int16, int16, int16, int16,
                                  int16, int16, int16)
        __asm__("__real__Z29DoRWStuffStartOfFrame_Horizonsssssss");
extern "C" bool RealPlainTarget(int16, int16, int16, int16,
                                int16, int16, int16)
        __asm__("__real__Z21DoRWStuffStartOfFramesssssss");

extern "C" bool AnonymousRealStartHorizon(int16, int16, int16, int16,
                                          int16, int16, int16)
        __asm__("_ZN12_GLOBAL__N_116RealStartHorizonEsssssss");
extern "C" bool AnonymousRealStartPlain(int16, int16, int16, int16,
                                        int16, int16, int16)
        __asm__("_ZN12_GLOBAL__N_114RealStartPlainEsssssss");

extern "C" bool AnonymousRealStartHorizon(
        int16 a, int16 b, int16 c, int16 d, int16 e, int16 f, int16 g) {
    return RealHorizonTarget(a, b, c, d, e, f, g);
}

extern "C" bool AnonymousRealStartPlain(
        int16 a, int16 b, int16 c, int16 d, int16 e, int16 f, int16 g) {
    return RealPlainTarget(a, b, c, d, e, f, g);
}

#endif
