// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

package org.hyprism.game;

import android.os.Bundle;
import android.os.PowerManager;
import android.util.Log;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.nio.file.Files;

/**
 * Hosts the macOS Hytale client, loaded natively by libmain.so. The Hyprism
 * launcher passes the authenticated client arguments in an intent extra.
 */
public class HytaleActivity extends SDLActivity {
    static final String ARGUMENTS_EXTRA = "org.hyprism.game.ARGUMENTS";
    /** The launcher instance to run; libmain.so reads it as HYPRISM_GAME_DIR (default: the files folder). */
    static final String GAME_DIR_EXTRA = "org.hyprism.game.GAME_DIR";
    private static volatile boolean running;

    /** Whether the game is open, so the world must not be pre-generated now. */
    static boolean isRunning() {
        return running;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        String gameDir = getIntent() != null ? getIntent().getStringExtra(GAME_DIR_EXTRA) : null;
        try {
            if (gameDir != null)
                android.system.Os.setenv("HYPRISM_GAME_DIR", gameDir, true);
            else
                android.system.Os.unsetenv("HYPRISM_GAME_DIR");
        } catch (android.system.ErrnoException e) {
            Log.w("HytaleHost", "could not pass the game folder: " + e);
        }
        super.onCreate(savedInstanceState);
        running = true;
        clearLeftoverPregeneration();
        // Draw under the camera cutout instead of leaving a black bar.
        getWindow().getAttributes().layoutInDisplayCutoutMode =
            WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        getWindow().setDecorFitsSystemWindows(false);
        tunePerformance();
        installServerLauncher();

        // On-screen gamepad over the game; the saved on/off state is restored
        // once the client has had time to initialise SDL.
        GamepadOverlay gamepad = new GamepadOverlay(this);
        addContentView(gamepad, new ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        gamepad.postDelayed(gamepad::restore, 15000);
    }

    /**
     * Keeps the phone out of the slow paths that were measured on a Galaxy S23: the battery saver
     * pinned the GPU at 220 MHz (out of 719) and cut the average frame rate by about a third, and a
     * hot SoC throttles it further. Nothing here changes what the game draws.
     */
    private void tunePerformance() {
        // Ask the vendor firmware for clocks it can hold, instead of short boosts that end in throttling.
        try {
            getWindow().setSustainedPerformanceMode(true);
        } catch (RuntimeException e) {
            Log.w("HytaleHost", "sustained performance mode unavailable: " + e);
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        // A steady 60 Hz beats a 120 Hz panel that drops to 45 under load. HYPRISM_REFRESH=0 leaves it to the system.
        float hz = 60f;
        try {
            String value = envValue("HYPRISM_REFRESH");
            if (value != null)
                hz = Float.parseFloat(value.trim());
        } catch (NumberFormatException e) {
            Log.w("HytaleHost", "ignoring a bad HYPRISM_REFRESH: " + e.getMessage());
        }
        if (hz > 0) {
            WindowManager.LayoutParams attributes = getWindow().getAttributes();
            attributes.preferredRefreshRate = hz;
            getWindow().setAttributes(attributes);
        }

        PowerManager power = getSystemService(PowerManager.class);
        if (power == null)
            return;
        if (power.isPowerSaveMode())
            Toast.makeText(this, "Battery saver is on: Hytale will run much slower. Turn it off for smooth play.",
                Toast.LENGTH_LONG).show();
        power.addThermalStatusListener(getMainExecutor(), status -> {
            Log.i("HytaleHost", "thermal status " + status);
            if (status >= PowerManager.THERMAL_STATUS_SEVERE)
                Toast.makeText(this, "The phone is hot and slowing down. Let it cool for a moment.",
                    Toast.LENGTH_SHORT).show();
        });
    }

    /** Brings files/java up to date when the game is opened without SetupActivity (adb, old shortcuts). */
    private void installServerLauncher() {
        new ServerSetup(this, message -> { }).installServerLauncher();
    }

    @Override
    protected void onDestroy() {
        running = false;
        super.onDestroy();
    }

    /**
     * PregenerateActivity sets ChunkConfig.PregenerateRegion only while it runs; if it was killed, the
     * region would stay and the server would generate it during play. Remove any that was left.
     */
    private void clearLeftoverPregeneration() {
        File[] saves = new File(getExternalFilesDir(null), "UserData/Saves").listFiles();
        if (saves == null)
            return;
        for (File save : saves) {
            File config = new File(save, "universe/worlds/default/config.json");
            try {
                if (!config.isFile())
                    continue;
                String text = new String(Files.readAllBytes(config.toPath()), java.nio.charset.StandardCharsets.UTF_8);
                if (!text.contains("PregenerateRegion"))
                    continue;
                org.json.JSONObject json = new org.json.JSONObject(text);
                org.json.JSONObject chunk = json.optJSONObject("ChunkConfig");
                if (chunk != null) {
                    chunk.remove("PregenerateRegion");
                    Files.write(config.toPath(), json.toString(2).getBytes(java.nio.charset.StandardCharsets.UTF_8));
                    Log.i("HytaleHost", "removed a leftover pre-generation region from " + save.getName());
                }
            } catch (Exception e) {
                Log.w("HytaleHost", "could not check " + config + ": " + e);
            }
        }
    }

    /** Reads NAME=value from the same env.txt the native loader reads, or null when it is absent. */
    private String envValue(String name) {
        File file = new File(getExternalFilesDir(null), "env.txt");
        try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
            String line;
            while ((line = reader.readLine()) != null)
                if (line.startsWith(name + "="))
                    return line.substring(name.length() + 1);
        } catch (java.io.IOException e) {
            // No env.txt: use the default.
        }
        return null;
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus)
            hideSystemBars();
    }

    // The status and navigation bars come back for a moment on a swipe from the edge.
    private void hideSystemBars() {
        WindowInsetsController controller = getWindow().getInsetsController();
        if (controller == null)
            return;
        controller.hide(WindowInsets.Type.systemBars());
        controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected String[] getArguments() {
        String[] arguments = getIntent() != null ? getIntent().getStringArrayExtra(ARGUMENTS_EXTRA) : null;
        return arguments != null ? arguments : new String[0];
    }
}
