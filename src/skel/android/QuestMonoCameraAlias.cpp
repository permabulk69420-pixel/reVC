#if defined(ANDROID)

#include "common.h"
#include "rwcore.h"

// The stereo integration translation unit still contains its old wrapper for
// future per-eye work. The mono baseline deliberately does not enable the
// linker's psCameraBeginUpdate wrap, so normal reVC calls go straight to the
// real RenderWare function. This alias only satisfies the dormant wrapper's
// internal __real_* reference at link time.
extern "C" RwBool psCameraBeginUpdate(RwCamera* camera);

extern "C" RwBool __real_psCameraBeginUpdate(RwCamera* camera) {
    return psCameraBeginUpdate(camera);
}

#endif
