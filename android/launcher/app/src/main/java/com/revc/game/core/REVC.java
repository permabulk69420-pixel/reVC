package com.revc.game.core;

/** JNI bridge used by the Android launcher to configure the native game. */
public final class REVC {
    private REVC() {
    }

    public static native void setGamePath(String path);
}
