package jp.xenia.emulator;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

public class LauncherActivity extends Activity {
    private static final int REQUEST_OPEN_GPU_TRACE_VIEWER = 0;
    private static final int REQUEST_READ_STORAGE = 1;
    private static final String PREFS_NAME = "aetherx360_game_settings";
    private static final String PREFS_GAME_FOLDER = "aetherx360_game_folder";

    private final List<GameEntry> gameEntries = new ArrayList<>();

    @Override
    protected void onCreate(final Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_launcher);
        final String savedFolder = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                .getString(PREFS_GAME_FOLDER, "");
        final EditText folderInput = findViewById(R.id.game_folder_location);
        if (!savedFolder.isEmpty()) {
            folderInput.setText(savedFolder);
        }
        folderInput.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(final CharSequence s, final int start, final int count, final int after) {
            }

            @Override
            public void onTextChanged(final CharSequence s, final int start, final int before, final int count) {
                getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                        .putString(PREFS_GAME_FOLDER, s.toString().trim())
                        .apply();
            }

            @Override
            public void afterTextChanged(final Editable s) {
                scanGames();
            }
        });
        scanGames();
    }

    @Override
    protected void onActivityResult(
            final int requestCode, final int resultCode, final Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_OPEN_GPU_TRACE_VIEWER && resultCode == RESULT_OK) {
            final Uri uri = data.getData();
            if (uri != null) {
                final Intent gpuTraceViewerIntent = new Intent(this, GpuTraceViewerActivity.class);
                final Bundle gpuTraceViewerLaunchArguments = new Bundle();
                gpuTraceViewerLaunchArguments.putString("target_trace_file", uri.toString());
                gpuTraceViewerIntent.putExtra(
                        WindowedAppActivity.EXTRA_CVARS, gpuTraceViewerLaunchArguments);
                startActivity(gpuTraceViewerIntent);
            }
        }
    }

    @Override
    public void onRequestPermissionsResult(final int requestCode, final String[] permissions,
                                           final int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_READ_STORAGE
                && grantResults.length > 0
                && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            scanGames();
        } else {
            Toast.makeText(this, "Storage permission is optional, but scanning external game folders may be limited.", Toast.LENGTH_LONG).show();
            scanGames();
        }
    }

    public void onScanGamesClick(final View view) {
        scanGames();
    }

    public void onLaunchGpuTraceViewerClick(final View view) {
        final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("application/octet-stream");
        startActivityForResult(intent, REQUEST_OPEN_GPU_TRACE_VIEWER);
    }

    public void onLaunchWindowDemoClick(final View view) {
        startActivity(new Intent(this, WindowDemoActivity.class));
    }

    private void scanGames() {
        gameEntries.clear();
        final LinearLayout container = findViewById(R.id.game_list_container);
        container.removeAllViews();

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M
                && checkSelfPermission(android.Manifest.permission.READ_EXTERNAL_STORAGE)
                != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{android.Manifest.permission.READ_EXTERNAL_STORAGE}, REQUEST_READ_STORAGE);
            return;
        }

        final EditText folderInput = findViewById(R.id.game_folder_location);
        final String configuredFolder = folderInput.getText().toString().trim();
        final List<File> rootDirs = new ArrayList<>();

        if (!configuredFolder.isEmpty()) {
            final File configuredDir = new File(configuredFolder);
            if (configuredDir.exists()) {
                rootDirs.add(configuredDir);
            }
        }

        if (rootDirs.isEmpty()) {
            rootDirs.add(getExternalFilesDir(null));
            rootDirs.add(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS));
            rootDirs.add(new File(Environment.getExternalStorageDirectory(), "Games"));
            rootDirs.add(new File(Environment.getExternalStorageDirectory(), "Xbox 360"));
            rootDirs.add(new File(Environment.getExternalStorageDirectory(), "xbox360"));
            rootDirs.add(new File(Environment.getExternalStorageDirectory(), "AetherX360"));
        }

        for (File rootDir : rootDirs) {
            if (rootDir == null || !rootDir.exists()) {
                continue;
            }
            addGamesFromDirectory(rootDir);
        }

        Collections.sort(gameEntries, (left, right) -> left.displayName.compareToIgnoreCase(right.displayName));
        if (gameEntries.isEmpty()) {
            final TextView emptyView = new TextView(this);
            emptyView.setText(R.string.empty_games);
            emptyView.setTextColor(0xFFD7E1F5);
            container.addView(emptyView);
            return;
        }

        for (GameEntry entry : gameEntries) {
            final View card = LayoutInflater.from(this).inflate(R.layout.game_card, container, false);
            final TextView title = card.findViewById(R.id.game_title);
            final TextView path = card.findViewById(R.id.game_path);
            final Button playButton = card.findViewById(R.id.game_play_button);
            final Button settingsButton = card.findViewById(R.id.game_settings_button);

            title.setText(entry.displayName);
            path.setText(entry.file.getAbsolutePath());
            playButton.setOnClickListener(v -> launchGame(entry));
            settingsButton.setOnClickListener(v -> showSettingsDialog(entry));
            container.addView(card);
        }
    }

    private void addGamesFromDirectory(final File directory) {
        final File[] files = directory.listFiles();
        if (files == null) {
            return;
        }

        final List<File> candidates = new ArrayList<>();
        for (File file : files) {
            if (file.isDirectory()) {
                addGamesFromDirectory(file);
                continue;
            }
            if (isSupportedGameFile(file)) {
                candidates.add(file);
            }
        }

        if (candidates.isEmpty()) {
            return;
        }

        final File selected = chooseSingleCandidate(candidates);
        if (selected != null) {
            gameEntries.add(new GameEntry(selected,
                    selected.getName().replaceFirst("\\.(apk|xex|iso|xiso|zar)$", "")));
        }
    }

    private boolean isSupportedGameFile(final File file) {
        final String lowerName = file.getName().toLowerCase();
        return lowerName.endsWith(".apk")
                || lowerName.endsWith(".xex")
                || lowerName.endsWith(".iso")
                || lowerName.endsWith(".xiso")
                || lowerName.endsWith(".zar");
    }

    private File chooseSingleCandidate(final List<File> candidates) {
        final List<File> apkCandidates = new ArrayList<>();
        for (File candidate : candidates) {
            if (candidate.getName().toLowerCase().endsWith(".apk")) {
                apkCandidates.add(candidate);
            }
        }
        if (!apkCandidates.isEmpty()) {
            Collections.sort(apkCandidates, (left, right) -> left.getName().compareToIgnoreCase(right.getName()));
            for (File candidate : apkCandidates) {
                final String name = candidate.getName().toLowerCase();
                if (name.contains("aetherx360") || name.contains("xenia")) {
                    return candidate;
                }
            }
            return apkCandidates.get(0);
        }
        Collections.sort(candidates, (left, right) -> left.getName().compareToIgnoreCase(right.getName()));
        return candidates.get(0);
    }

    private void launchGame(final GameEntry entry) {
        final Intent intent = new Intent(this, GameActivity.class);
        final Bundle launchArguments = new Bundle();
        launchArguments.putString("target", entry.file.getAbsolutePath());
        launchArguments.putString("gpu", getSetting(entry, "gpu", "any"));
        launchArguments.putString("apu", getSetting(entry, "apu", "any"));
        intent.putExtra(WindowedAppActivity.EXTRA_CVARS, launchArguments);
        startActivity(intent);
    }

    private void showSettingsDialog(final GameEntry entry) {
        final View dialogView = LayoutInflater.from(this).inflate(R.layout.dialog_game_settings, null);
        final Spinner gpuSpinner = dialogView.findViewById(R.id.game_gpu_spinner);
        final Spinner apuSpinner = dialogView.findViewById(R.id.game_apu_spinner);

        gpuSpinner.setSelection(getIndex(gpuSpinner, getSetting(entry, "gpu", "any")));
        apuSpinner.setSelection(getIndex(apuSpinner, getSetting(entry, "apu", "any")));

        new AlertDialog.Builder(this)
                .setTitle(getString(R.string.settings_header) + " - " + entry.displayName)
                .setView(dialogView)
                .setPositiveButton("Save", (dialog, which) -> {
                    saveSetting(entry, "gpu", gpuSpinner.getSelectedItem().toString());
                    saveSetting(entry, "apu", apuSpinner.getSelectedItem().toString());
                    Toast.makeText(this, "Settings saved for " + entry.displayName, Toast.LENGTH_SHORT).show();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private String getSetting(final GameEntry entry, final String key, final String defaultValue) {
        final SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        return prefs.getString(entry.file.getAbsolutePath() + "::" + key, defaultValue);
    }

    private void saveSetting(final GameEntry entry, final String key, final String value) {
        final SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        prefs.edit().putString(entry.file.getAbsolutePath() + "::" + key, value).apply();
    }

    private int getIndex(final Spinner spinner, final String value) {
        for (int index = 0; index < spinner.getCount(); ++index) {
            if (spinner.getItemAtPosition(index).toString().equalsIgnoreCase(value)) {
                return index;
            }
        }
        return 0;
    }

    private static final class GameEntry {
        private final File file;
        private final String displayName;

        private GameEntry(final File file, final String displayName) {
            this.file = file;
            this.displayName = displayName;
        }
    }
}
