#if defined(ANDROID)

// Diagnostic controller build: leave CPad::UpdatePads() completely untouched so
// reVC's stock SDL/Android controller path remains authoritative. Quest Touch
// injection can be restored later as a non-replacing hook once normal controller
// input has been verified on hardware.

#endif
