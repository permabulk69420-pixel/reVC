package com.revc.game;

import androidx.appcompat.app.AppCompatActivity;

import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;

/**
 * Minimal flat-Android bring-up launcher.
 *
 * It asks for the folder containing the user's legally owned Vice City data,
 * validates a few required files, then starts the native SDL/reVC activity.
 */
public final class MainActivity extends AppCompatActivity {
    private static final String PREFS = "revc_launcher";
    private static final String PREF_GAME_PATH = "game_path";
    private static final String DEFAULT_GAME_PATH = "/storage/emulated/0/reVC";

    private EditText pathField;
    private TextView statusText;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        int padding = (int) (20 * getResources().getDisplayMetrics().density);

        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(padding, padding, padding, padding);

        TextView title = new TextView(this);
        title.setText("reVC Android bring-up");
        title.setTextSize(24);
        content.addView(title, matchWrap());

        TextView instructions = new TextView(this);
        instructions.setText("Enter the folder containing models, data, audio and TEXT. The default is /storage/emulated/0/reVC.");
        instructions.setPadding(0, padding / 2, 0, padding / 2);
        content.addView(instructions, matchWrap());

        pathField = new EditText(this);
        pathField.setSingleLine(true);
        pathField.setText(getSharedPreferences(PREFS, MODE_PRIVATE)
                .getString(PREF_GAME_PATH, DEFAULT_GAME_PATH));
        pathField.setHint("/storage/emulated/0/reVC");
        content.addView(pathField, matchWrap());

        Button permissionButton = new Button(this);
        permissionButton.setText("Grant file access");
        permissionButton.setOnClickListener(v -> requestAllFilesAccess());
        content.addView(permissionButton, matchWrap());

        Button launchButton = new Button(this);
        launchButton.setText("Launch reVC");
        launchButton.setOnClickListener(v -> validateAndLaunch());
        content.addView(launchButton, matchWrap());

        statusText = new TextView(this);
        statusText.setPadding(0, padding / 2, 0, 0);
        content.addView(statusText, matchWrap());

        ScrollView scrollView = new ScrollView(this);
        scrollView.addView(content);
        setContentView(scrollView);

        refreshPermissionStatus();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (statusText != null) {
            refreshPermissionStatus();
        }
    }

    private LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private boolean hasRawFileAccess() {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager();
    }

    private void requestAllFilesAccess() {
        if (hasRawFileAccess()) {
            statusText.setText("File access is already granted.");
            return;
        }

        try {
            Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
            intent.setData(Uri.parse("package:" + getPackageName()));
            startActivity(intent);
        } catch (Exception ignored) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    private void refreshPermissionStatus() {
        if (hasRawFileAccess()) {
            statusText.setText("File access: granted");
        } else {
            statusText.setText("File access is required before launching.");
        }
    }

    private void validateAndLaunch() {
        if (!hasRawFileAccess()) {
            statusText.setText("Grant file access first.");
            requestAllFilesAccess();
            return;
        }

        String gamePath = pathField.getText().toString().trim();
        File root = new File(gamePath);
        StringBuilder missing = new StringBuilder();
        requireFile(root, "models/gta3.img", missing);
        requireFile(root, "data/main.scm", missing);
        requireDirectory(root, "audio", missing);
        requireDirectory(root, "TEXT", missing);

        if (missing.length() > 0) {
            statusText.setText("Wrong/incomplete Vice City folder. Missing:\n" + missing);
            return;
        }

        getSharedPreferences(PREFS, MODE_PRIVATE)
                .edit()
                .putString(PREF_GAME_PATH, root.getAbsolutePath())
                .apply();

        Intent intent = new Intent(this, GameActivity.class);
        intent.putExtra(GameActivity.EXTRA_GAME_PATH, root.getAbsolutePath());
        startActivity(intent);
    }

    private void requireFile(File root, String relativePath, StringBuilder missing) {
        if (!new File(root, relativePath).isFile()) {
            missing.append(relativePath).append('\n');
        }
    }

    private void requireDirectory(File root, String relativePath, StringBuilder missing) {
        if (!new File(root, relativePath).isDirectory()) {
            missing.append(relativePath).append("/\n");
        }
    }
}
