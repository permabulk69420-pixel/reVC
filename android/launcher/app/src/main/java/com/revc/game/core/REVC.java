package com.revc.game.core;

import android.app.Activity;

/** Small JNI bridge for path setup and SDL/OpenXR lifecycle ownership. */
public final class REVC {
    private REVC() {}

    public static native void initialize(Activity activity, String path);
    public static native void requestExit();
    public static native boolean isOpenXrPresentationActive();
}
