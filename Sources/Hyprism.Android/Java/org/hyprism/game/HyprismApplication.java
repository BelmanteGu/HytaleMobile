// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

package org.hyprism.game;

import android.app.Application;

/**
 * The app's Application. The launcher (Avalonia on .NET) starts only in the main process; the game
 * runs in the ":game" process, where .NET is never loaded: the Mach-O loader hosts the client's own
 * runtime there, and the launcher's would only cost memory and contend for its signal handlers.
 */
public class HyprismApplication extends org.hyprism.launcher.MainApplication {
    @Override
    public void onCreate() {
        if (Application.getProcessName().endsWith(":game"))
            return;
        super.onCreate();
    }
}
