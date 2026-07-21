package com.revc.game;

import android.os.Bundle;
import android.util.Log;

import org.libsdl.app.SDLActivity;

/**
 * Real native entry point for the Android reVC build.
 *
 * The old MainActivity was only the Android Studio "Hello World" placeholder.
 * This activity starts SDL and loads the native game libraries.
 */
public final class GameActivity extends SDLActivity {
    private static final String TAG = "reVC";

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "openal", "reVC" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        Log.i(TAG, "Starting native reVC activity");
        super.onCreate(savedInstanceState);
    }
}
