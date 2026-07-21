package com.revc.game;

import android.content.ComponentName;
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
 * Immersive shell around the known-good flat SDL/reVC renderer.
 * SDL creates the real Android window and EGL context first; OpenXR adopts that
 * existing context only after RenderWare has created a valid camera target.
 */
public final class GameActivity extends SDLActivity {
    public static final String EXTRA_GAME_PATH = "com.revc.game.GAME_PATH";
    private static final String ACTION_ENTER_IMMERSIVE =
            "com.revc.game.action.ENTER_IMMERSIVE";
    private static final String EXTRA_IMMERSIVE_HANDOFF =
            "com.revc.game.IMMERSIVE_HANDOFF";
    private static final String IMMERSIVE_ALIAS =
            "com.revc.game.ImmersiveGameAlias";
    private static final String TAG = "reVC-XR";

    private boolean immersiveAliasAcknowledged;

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "openal", "openxr_loader", "reVC"};
    }

    /**
     * SDLActivity invokes this before SDL.setupJNI(), surface creation and the
     * SDL_main thread. Configure storage and give the OpenXR bridge an explicit
     * global reference to this real GameActivity. SDL2 still owns its own JNI
     * setup; this is only the Android context required by the OpenXR loader.
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
                    "JAVA OpenXR Activity bridge configured before SDL surface startup");
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
        Log.i(TAG, "Starting normal SDL/reVC renderer before OpenXR handoff");
        super.onCreate(savedInstanceState);
        if (!mBrokenLibraries) {
            appendBootstrapLog(gamePath(),
                    "JAVA SDL JNI setup complete; creating normal Android render surface");
            Log.i(TAG, "SDL JNI setup completed before surface creation");
        }
        acknowledgeImmersiveAlias(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        acknowledgeImmersiveAlias(intent);
    }

    /**
     * Called by the SDL render thread after OpenXR has created a session and
     * parked the existing EGL context. Launching the alias targets this same
     * singleTask Activity, so Android delivers onNewIntent instead of creating
     * another renderer or another native thread.
     */
    public void requestImmersiveHandoff() {
        runOnUiThread(() -> {
            appendBootstrapLog(gamePath(),
                    "JAVA launching existing GameActivity through IMMERSIVE_HMD alias");
            try {
                Intent intent = new Intent(ACTION_ENTER_IMMERSIVE);
                intent.setComponent(new ComponentName(getPackageName(), IMMERSIVE_ALIAS));
                intent.putExtra(EXTRA_IMMERSIVE_HANDOFF, true);
                intent.putExtra(EXTRA_GAME_PATH, gamePath());
                intent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP
                        | Intent.FLAG_ACTIVITY_SINGLE_TOP
                        | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
                startActivity(intent);
            } catch (RuntimeException error) {
                appendBootstrapLog(gamePath(),
                        "JAVA immersive alias launch failed: "
                                + error.getClass().getName() + ": "
                                + String.valueOf(error.getMessage()));
                Log.e(TAG, "Unable to launch immersive Activity alias", error);
            }
        });
    }

    private void acknowledgeImmersiveAlias(Intent intent) {
        if (immersiveAliasAcknowledged || intent == null) {
            return;
        }
        if (!intent.getBooleanExtra(EXTRA_IMMERSIVE_HANDOFF, false)
                && !ACTION_ENTER_IMMERSIVE.equals(intent.getAction())) {
            return;
        }

        immersiveAliasAcknowledged = true;
        appendBootstrapLog(gamePath(),
                "JAVA IMMERSIVE_HMD alias returned to existing GameActivity");
        REVC.notifyImmersiveAliasReady();
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

    private static void appendBootstrapLog(String gamePath, String message) {
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
