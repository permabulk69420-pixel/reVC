package com.revc.game;

import android.content.Context;
import android.os.Bundle;
import android.os.Environment;
import android.util.Log;
import android.view.SurfaceHolder;

import com.revc.game.core.REVC;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import java.io.File;

/**
 * Immersive shell around the known-good flat SDL/reVC renderer.
 * SDL creates the real Android window and EGL context first; OpenXR adopts that
 * existing context only after RenderWare has created a valid camera target.
 */
public final class GameActivity extends SDLActivity {
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
    protected SDLSurface createSDLSurface(Context context) {
        return new QuestSurface(context);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        Log.i(TAG, "Starting normal SDL/reVC renderer before OpenXR handoff");
        super.onCreate(savedInstanceState);
        if (!mBrokenLibraries) {
            REVC.initialize(this, gamePath());
            Log.i(TAG, "Native bridge configured for existing SDL EGL context");
        }
    }

    @Override
    protected void pauseNativeThread() {
        if (xrOwnsPresentation() && !isFinishing() && !isDestroyed()) {
            Log.i(TAG, "Ignoring Android pause after OpenXR took presentation ownership");
            return;
        }
        super.pauseNativeThread();
    }

    @Override
    protected void resumeNativeThread() {
        if (xrOwnsPresentation() && !isFinishing() && !isDestroyed()) {
            return;
        }
        super.resumeNativeThread();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        if (xrOwnsPresentation() && !isFinishing() && !isDestroyed()) {
            Log.i(TAG, "OpenXR owns focus; ignoring Android window focus=" + hasFocus);
            return;
        }
        super.onWindowFocusChanged(hasFocus);
    }

    private boolean xrOwnsPresentation() {
        if (mBrokenLibraries) {
            return false;
        }
        try {
            return REVC.isOpenXrPresentationActive();
        } catch (UnsatisfiedLinkError error) {
            return false;
        }
    }

    private String gamePath() {
        String path = getIntent().getStringExtra(EXTRA_GAME_PATH);
        if (path == null || path.trim().isEmpty()) {
            path = new File(Environment.getExternalStorageDirectory(), "reVC")
                    .getAbsolutePath();
        }
        return path.endsWith(File.separator) ? path : path + File.separator;
    }

    private final class QuestSurface extends SDLSurface {
        QuestSurface(Context context) {
            super(context);
        }

        @Override
        public void surfaceCreated(SurfaceHolder holder) {
            if (xrOwnsPresentation() && !isFinishing()) {
                Log.i(TAG, "Ignoring replacement Android surface while OpenXR is active");
                return;
            }
            super.surfaceCreated(holder);
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            if (xrOwnsPresentation() && !isFinishing()) {
                mWidth = Math.max(1, width);
                mHeight = Math.max(1, height);
                return;
            }
            super.surfaceChanged(holder, format, width, height);
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
            if (xrOwnsPresentation() && !isFinishing()) {
                Log.i(TAG, "Android surface retired; SDL thread remains on its parked EGL context");
                mIsSurfaceReady = false;
                return;
            }
            super.surfaceDestroyed(holder);
        }
    }
}
