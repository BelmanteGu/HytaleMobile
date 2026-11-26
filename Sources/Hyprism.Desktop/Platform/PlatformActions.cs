// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

using System.Globalization;

namespace Hyprism.Desktop.Platform;

/// <summary>
/// Actions only some platforms offer. A platform head sets them at startup; views show the matching
/// commands only when they are set.
/// </summary>
public static class PlatformActions
{
    /// <summary>Opens the game host's world pre-generation screen (Android).</summary>
    public static Action? PregenerateWorld { get; set; }

    /// <summary>Whether <see cref="PregenerateWorld"/> is available here.</summary>
    public static bool CanPregenerateWorld => PregenerateWorld is not null;

    /// <summary>The menu label for <see cref="PregenerateWorld"/>.</summary>
    public static string PregenerateWorldLabel =>
        CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "pt" ? "Pré-gerar mundo" : "Pre-generate world";
}
