package com.revc.game.core;

import android.app.Activity;

/** JNI bridge used by the Android launcher and the Quest renderer handoff. */
public final class REVC {
    private REVC() {
    }

    public static native void setGamePath(String path);
    public static native void initialize(Activity activity, String path);
    public static native void registerImmersiveHost(Activity activity);
    public static native boolean isOpenXrHandoffActive();
    public static native boolean isOpenXrPresentationActive();
}
