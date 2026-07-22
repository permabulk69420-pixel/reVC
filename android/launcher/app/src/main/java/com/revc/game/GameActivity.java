package com.revc.game;

import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.Environment;
import android.util.Log;
import android.view.SurfaceHolder;

import com.revc.game.core.REVC;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;

/**
 * Flat bootstrap shell around the known-good SDL/reVC renderer.
 *
 * SDL creates the real Android window and EGL context first. Native code then
 * parks that exact context on a pbuffer before asking this Activity to launch
 * the real IMMERSIVE_HMD host. The SDL thread keeps running while the host owns
 * Android focus and OpenXR owns presentation.
 */
public final class GameActivity extends SDLActivity {
    public static final String EXTRA_GAME_PATH = "com.revc.game.GAME_PATH";
    static final String ACTION_ENTER_IMMERSIVE =
            "com.revc.game.action.ENTER_IMMERSIVE";
    private static final String TAG = "reVC-XR";

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "openal", "openxr_loader", "reVC"};
    }

    /**
     * SDLActivity invokes this before SDL.setupJNI(), surface creation and the
     * SDL_main thread. Configure storage and give the native bridge an explicit
     * global reference to this flat bootstrap Activity.
     */
    @Override
    public void loadLibraries() {
        final String path = gamePath();
        appendBootstrapLog(path, "JAVA GameActivity entered library-load stage");
        try {
            super.loadLibraries();
            appendBootstrapLog(path, "JAVA native libraries loaded");
            REVC.setGamePath(path);
            REVC.initialize(this, path);
            appendBootstrapLog(path,
                    "JAVA flat GameActivity bridge configured before SDL surface startup");
        } catch (Throwable error) {
            appendBootstrapLog(path, "JAVA startup failed before SDL surface: "
                    + error.getClass().getName() + ": " + String.valueOf(error.getMessage()));
            throw error;
        }
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
        Log.i(TAG, "Starting normal SDL/reVC renderer before immersive host handoff");
        super.onCreate(savedInstanceState);
        if (!mBrokenLibraries) {
            appendBootstrapLog(gamePath(),
                    "JAVA SDL JNI setup complete; creating normal Android render surface");
            Log.i(TAG, "SDL JNI setup completed before surface creation");
        }
    }

    /**
     * Called by the SDL render thread only after its existing EGL context has
     * been rebound to a pbuffer. This starts a separate real immersive Activity;
     * it does not recreate SDL, RenderWare, or the game thread.
     */
    public void requestImmersiveHandoff() {
        runOnUiThread(() -> {
            appendBootstrapLog(gamePath(),
                    "JAVA launching real IMMERSIVE_HMD host after SDL context park");
            try {
                Intent intent = new Intent(this, ImmersiveHostActivity.class);
                intent.setAction(ACTION_ENTER_IMMERSIVE);
                intent.putExtra(EXTRA_GAME_PATH, gamePath());
                intent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP
                        | Intent.FLAG_ACTIVITY_SINGLE_TOP);
                startActivity(intent);
            } catch (RuntimeException error) {
                appendBootstrapLog(gamePath(),
                        "JAVA immersive host launch failed: "
                                + error.getClass().getName() + ": "
                                + String.valueOf(error.getMessage()));
                Log.e(TAG, "Unable to launch immersive host Activity", error);
            }
        });
    }

    /** Native shutdown callback used by the existing Android wrapper. */
    public void exitGame() {
        runOnUiThread(() -> {
            if (!isFinishing()) {
                finish();
            }
        });
    }

    @Override
    protected void pauseNativeThread() {
        if (xrOwnsOrIsTakingPresentation() && !isFinishing() && !isDestroyed()) {
            Log.i(TAG, "Ignoring Android pause during immersive handoff/OpenXR ownership");
            return;
        }
        super.pauseNativeThread();
    }

    @Override
    protected void resumeNativeThread() {
        if (xrOwnsOrIsTakingPresentation() && !isFinishing() && !isDestroyed()) {
            return;
        }
        super.resumeNativeThread();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        if (xrOwnsOrIsTakingPresentation() && !isFinishing() && !isDestroyed()) {
            Log.i(TAG, "Immersive handoff/OpenXR owns focus; ignoring Android window focus="
                    + hasFocus);
            return;
        }
        super.onWindowFocusChanged(hasFocus);
    }

    private boolean xrOwnsOrIsTakingPresentation() {
        if (mBrokenLibraries) {
            return false;
        }
        try {
            return REVC.isOpenXrHandoffActive()
                    || REVC.isOpenXrPresentationActive();
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

    static void appendBootstrapLog(String gamePath, String message) {
        try {
            File root = new File(gamePath);
            File userFiles = new File(root, "userfiles");
            if (!userFiles.isDirectory() && !userFiles.mkdirs()) {
                Log.e(TAG, "Unable to create Java bootstrap log directory: " + userFiles);
                return;
            }
            try (FileWriter writer = new FileWriter(new File(userFiles, "xr_log.txt"), true)) {
                writer.write(message);
                writer.write('\n');
                writer.flush();
            }
        } catch (IOException | RuntimeException error) {
            Log.e(TAG, "Unable to append Java bootstrap log", error);
        }
    }

    private final class QuestSurface extends SDLSurface {
        QuestSurface(Context context) {
            super(context);
        }

        @Override
        public void surfaceCreated(SurfaceHolder holder) {
            if (xrOwnsOrIsTakingPresentation() && !isFinishing()) {
                Log.i(TAG, "Ignoring replacement Android surface during immersive ownership");
                return;
            }
            super.surfaceCreated(holder);
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            if (xrOwnsOrIsTakingPresentation() && !isFinishing()) {
                mWidth = Math.max(1, width);
                mHeight = Math.max(1, height);
                return;
            }
            super.surfaceChanged(holder, format, width, height);
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
            if (xrOwnsOrIsTakingPresentation() && !isFinishing()) {
                Log.i(TAG, "Flat Android surface retired; SDL remains on its parked EGL context");
                mIsSurfaceReady = false;
                return;
            }
            super.surfaceDestroyed(holder);
        }
    }
}
