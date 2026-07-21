package com.revc.game;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;

/** A normal flat launcher. It owns no native, EGL or OpenXR state. */
public final class MainActivity extends Activity {
    private static final int LEGACY_STORAGE_REQUEST = 41;
    private static final String GAME_DIRECTORY = "reVC";

    private boolean firstResume = true;
    private boolean permissionScreenOpened;
    private boolean gameStarted;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        continueToGame();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (firstResume) {
            firstResume = false;
            return;
        }
        if (permissionScreenOpened && !gameStarted) {
            permissionScreenOpened = false;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
                    !Environment.isExternalStorageManager()) {
                showStorageError();
            } else {
                continueToGame();
            }
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == LEGACY_STORAGE_REQUEST) {
            if (grantResults.length > 0 &&
                    grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                continueToGame();
            } else {
                showStorageError();
            }
        }
    }

    private void continueToGame() {
        if (gameStarted) {
            return;
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (!Environment.isExternalStorageManager()) {
                if (!permissionScreenOpened) {
                    permissionScreenOpened = true;
                    Intent permission = new Intent(
                            Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                            Uri.parse("package:" + getPackageName()));
                    try {
                        startActivity(permission);
                    } catch (Exception ignored) {
                        startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                    }
                } else {
                    showStorageError();
                }
                return;
            }
        } else if (checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] {
                    Manifest.permission.READ_EXTERNAL_STORAGE,
                    Manifest.permission.WRITE_EXTERNAL_STORAGE
            }, LEGACY_STORAGE_REQUEST);
            return;
        }

        File gameRoot = new File(Environment.getExternalStorageDirectory(), GAME_DIRECTORY);
        File userFiles = new File(gameRoot, "userfiles");
        if ((!userFiles.isDirectory() && !userFiles.mkdirs()) || !userFiles.canWrite()) {
            showStorageError();
            return;
        }

        gameStarted = true;
        Intent game = new Intent(this, GameActivity.class);
        game.setAction(GameActivity.ACTION_QUEST_GAME);
        game.putExtra(GameActivity.EXTRA_GAME_PATH, gameRoot.getAbsolutePath() + File.separator);
        startActivity(game);
        finish();
    }

    private void showStorageError() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setPadding(48, 48, 48, 48);
        layout.setBackgroundColor(Color.BLACK);

        TextView message = new TextView(this);
        message.setTextColor(Color.WHITE);
        message.setTextSize(18);
        message.setGravity(Gravity.CENTER);
        message.setText("reVC needs file access to read /storage/emulated/0/reVC.");
        layout.addView(message);

        Button retry = new Button(this);
        retry.setText("Grant file access");
        retry.setOnClickListener(view -> {
            permissionScreenOpened = false;
            continueToGame();
        });
        layout.addView(retry);
        setContentView(layout);
    }
}
