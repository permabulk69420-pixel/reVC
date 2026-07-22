#if defined(ANDROID)

// The mono baseline now wraps psCameraBeginUpdate directly so it can redirect
// drawing into a raw GLES capture FBO after RenderWare has performed its normal
// camera setup. No __real_* compatibility alias is required.

#endif
