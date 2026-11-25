// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

package org.hyprism.game;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * The phone options of the game, kept in the game's own process and changed from the in-game panel
 * (GameSettingsPanel). Resolution, battery mode and lighter textures are read when the game starts,
 * so changing them asks for a restart; the control size and the pre-generation apply at once.
 */
final class GameSettings {
    /** The 3D view heights the panel offers; 0 keeps the game's own render scale. */
    static final int[] RESOLUTIONS = { 0, 540, 480, 360, 240 };

    private static int startedHeight = -1;
    private static boolean startedBattery, startedHalf;

    private GameSettings() {
    }

    private static SharedPreferences prefs(Context c) {
        return c.getApplicationContext().getSharedPreferences("game-settings", Context.MODE_PRIVATE);
    }

    static int renderHeight(Context c) {
        return prefs(c).getInt("renderHeight", 0);
    }

    static boolean batterySaver(Context c) {
        return prefs(c).getBoolean("batterySaver", false);
    }

    static boolean halfTextures(Context c) {
        return prefs(c).getBoolean("halfTextures", false);
    }

    static boolean pregenerate(Context c) {
        return prefs(c).getBoolean("pregenerate", true);
    }

    /** Scale of the touch controls, 0.6 to 1.6. */
    static float controlSize(Context c) {
        return Math.max(0.6f, Math.min(1.6f, prefs(c).getFloat("controlSize", 1f)));
    }

    static void setRenderHeight(Context c, int height) {
        prefs(c).edit().putInt("renderHeight", height).apply();
    }

    static void setBatterySaver(Context c, boolean on) {
        prefs(c).edit().putBoolean("batterySaver", on).apply();
    }

    static void setHalfTextures(Context c, boolean on) {
        prefs(c).edit().putBoolean("halfTextures", on).apply();
    }

    static void setPregenerate(Context c, boolean on) {
        prefs(c).edit().putBoolean("pregenerate", on).apply();
    }

    static void setControlSize(Context c, float size) {
        prefs(c).edit().putFloat("controlSize", size).apply();
    }

    /** Remembers what this run of the game started with, to tell later whether a restart is needed. */
    static void snapshot(Context c) {
        startedHeight = renderHeight(c);
        startedBattery = batterySaver(c);
        startedHalf = halfTextures(c);
    }

    static boolean restartNeeded(Context c) {
        return startedHeight >= 0
            && (renderHeight(c) != startedHeight || batterySaver(c) != startedBattery
                || halfTextures(c) != startedHalf);
    }
}
