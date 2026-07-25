package com.revc.game;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;

import com.revc.game.core.REVC;

import java.io.File;

/**
 * Minimal real IMMERSIVE_HMD Activity used only as OpenXR's Android owner.
 *
 * It never creates SDL, EGL, RenderWare, or another game thread. The flat
 * GameActivity has already parked its existing EGL context before this Activity
 * is launched. This Activity simply becomes the foreground immersive owner and
 * hands its jobject to the native OpenXR loader/session creation path.
 */
public final class ImmersiveHostActivity extends Activity {
    private String gamePath;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
                | WindowManager.LayoutParams.FLAG_FULLSCREEN);
        getWindow().setStatusBarColor(Color.BLACK);
        getWindow().setNavigationBarColor(Color.BLACK);
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);

        FrameLayout blackRoot = new FrameLayout(this);
        blackRoot.setBackgroundColor(Color.BLACK);
        setContentView(blackRoot);

        gamePath = getIntent().getStringExtra(GameActivity.EXTRA_GAME_PATH);
        if (gamePath == null || gamePath.trim().isEmpty()) {
            gamePath = new File(android.os.Environment.getExternalStorageDirectory(), "reVC")
                    .getAbsolutePath() + File.separator;
        }
        if (!gamePath.endsWith(File.separator)) {
            gamePath += File.separator;
        }

        GameActivity.appendBootstrapLog(gamePath,
                "JAVA real IMMERSIVE_HMD host Activity created");
        try {
            REVC.registerImmersiveHost(this);
            GameActivity.appendBootstrapLog(gamePath,
                    "JAVA real IMMERSIVE_HMD host registered with native OpenXR bridge");
        } catch (Throwable error) {
            GameActivity.appendBootstrapLog(gamePath,
                    "JAVA immersive host registration failed: "
                            + error.getClass().getName() + ": "
                            + String.valueOf(error.getMessage()));
            finishAffinity();
        }
    }

    /** Native shutdown callback used by the existing Android wrapper. */
    public void exitGame() {
        runOnUiThread(this::finishAffinity);
    }

    @Override
    protected void onDestroy() {
        if (gamePath != null) {
            GameActivity.appendBootstrapLog(gamePath,
                    "JAVA real IMMERSIVE_HMD host Activity destroyed");
        }
        super.onDestroy();
    }
}
