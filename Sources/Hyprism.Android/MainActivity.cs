// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

using Android.App;
using Android.Content.PM;
using Android.OS;
using Android.Views;
using Avalonia.Android;

namespace Hyprism.Desktop.Android;

[Activity(
    Label = "Hytale",
    Theme = "@style/HyprismTheme.NoActionBar",
    Icon = "@drawable/icon",
    MainLauncher = true,
    ScreenOrientation = ScreenOrientation.SensorLandscape,
    ConfigurationChanges = ConfigChanges.Orientation | ConfigChanges.ScreenSize | ConfigChanges.UiMode)]
public sealed class MainActivity : AvaloniaMainActivity
{
    public override void OnWindowFocusChanged(bool hasFocus)
    {
        base.OnWindowFocusChanged(hasFocus);
        if (hasFocus)
            HideSystemBars();
    }

    // The launcher fills the screen like the game: the status and navigation bars come back for a
    // moment on a swipe from the edge.
    private void HideSystemBars()
    {
        if (Window is null || Build.VERSION.SdkInt < BuildVersionCodes.R)
            return;
        Window.SetDecorFitsSystemWindows(false);
        var controller = Window.InsetsController;
        if (controller is null)
            return;
        controller.Hide(WindowInsets.Type.SystemBars());
        controller.SystemBarsBehavior = (int)WindowInsetsControllerBehavior.ShowTransientBarsBySwipe;
    }
}
