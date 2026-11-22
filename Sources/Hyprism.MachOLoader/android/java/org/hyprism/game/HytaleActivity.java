// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

package org.hyprism.game;

import android.os.Bundle;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

/**
 * Hosts the macOS Hytale client, loaded natively by libmain.so. The Hyprism
 * launcher passes the authenticated client arguments in an intent extra.
 */
public class HytaleActivity extends SDLActivity {
    static final String ARGUMENTS_EXTRA = "org.hyprism.game.ARGUMENTS";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // Draw under the camera cutout instead of leaving a black bar.
        getWindow().getAttributes().layoutInDisplayCutoutMode =
            WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        getWindow().setDecorFitsSystemWindows(false);

        // On-screen gamepad over the game; the saved on/off state is restored
        // once the client has had time to initialise SDL.
        GamepadOverlay gamepad = new GamepadOverlay(this);
        addContentView(gamepad, new ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        gamepad.postDelayed(gamepad::restore, 15000);
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
