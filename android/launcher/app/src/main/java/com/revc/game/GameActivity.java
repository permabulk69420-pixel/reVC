package com.revc.game;

import android.os.Bundle;
import android.util.Log;

import org.libsdl.app.SDLActivity;

/** Native SDL entry point for the flat Android reVC build. */
public final class GameActivity extends SDLActivity {
    public static final String EXTRA_GAME_PATH = "com.revc.game.GAME_PATH";
    private static final String TAG = "reVC";

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "openal", "reVC" };
    }

    @Override
    protected String[] getArguments() {
        String gamePath = getIntent().getStringExtra(EXTRA_GAME_PATH);
        if (gamePath == null || gamePath.trim().isEmpty()) {
            return new String[0];
        }
        return new String[] { "--dir", gamePath };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        Log.i(TAG, "Starting native reVC activity");
        super.onCreate(savedInstanceState);
    }
}
