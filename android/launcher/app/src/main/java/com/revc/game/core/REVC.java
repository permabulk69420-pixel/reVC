package com.revc.game.core;

/** JNI bridge used by the Android launcher and the Quest renderer handoff. */
public final class REVC {
    private REVC() {
    }

    public static native void setGamePath(String path);
    public static native boolean isOpenXrPresentationActive();
}
