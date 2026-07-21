package com.revc.game;

import android.os.Bundle;
import android.os.Environment;
import android.util.Log;

import com.revc.game.core.REVC;

import org.libsdl.app.SDLActivity;

import java.io.File;

/** The sole immersive owner: GameActivity -> GameThread -> one EGL context -> OpenXR. */
public final class GameActivity extends SDLActivity {
    public static final String ACTION_QUEST_GAME = "com.revc.game.action.QUEST_GAME";
    public static final String EXTRA_GAME_PATH = "com.revc.game.GAME_PATH";
    private static final String TAG = "reVC-XR";

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "openal", "openxr_loader", "reVC"};
    }

    @Override
    protected String[] getArguments() {
        return new String[] {"--dir", gamePath()};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        Log.i(TAG, "GameActivity starting sole SDL/OpenXR owner");
        super.onCreate(savedInstanceState);
        if (!mBrokenLibraries) {
            REVC.initialize(this, gamePath());
            Log.i(TAG, "reVC JNI bridge has the real GameActivity; native startup is armed");
        }
    }

    @Override
    protected boolean shouldKeepNativeThreadRunning() {
        return !isFinishing() && !isDestroyed() && !mBrokenLibraries;
    }

    @Override
    protected boolean shouldStartNativeThreadWithoutSurface() {
        return true;
    }

    @Override
    protected boolean usesAndroidRenderSurface() {
        return false;
    }

    @Override
    protected long nativeThreadShutdownTimeoutMillis() {
        return 4000;
    }

    @Override
    protected boolean terminateProcessIfNativeThreadStalls() {
        return true;
    }

    private String gamePath() {
        String path = getIntent().getStringExtra(EXTRA_GAME_PATH);
        if (path == null || path.trim().isEmpty()) {
            path = new File(Environment.getExternalStorageDirectory(), "reVC")
                    .getAbsolutePath();
        }
        return path.endsWith(File.separator) ? path : path + File.separator;
    }
}
