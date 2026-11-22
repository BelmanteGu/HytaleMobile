// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

using System.Text.Json;
using Android.Content;
using Hyprism.Core.Game.Launch;
using Hyprism.Core.Infrastructure;

namespace Hyprism.Desktop.Android;

/// <summary>
/// Hands authenticated launches to the native game host app, which runs the
/// macOS arm64 client through the Hyprism Mach-O loader.
/// </summary>
internal sealed class AndroidGameHost : IExternalGameHost
{
    internal const string HostPackage = "org.hyprism.game";
    internal const string HostActivity = "org.hyprism.game.HytaleActivity";
    internal const string ArgumentsExtra = "org.hyprism.game.ARGUMENTS";

    // The host keeps the client files, so the launcher instance only marks the game as installed.
    private const string HostInstanceId = "6f8d1c2a-4b7e-4a3f-9c55-0a1b2c3d4e5f";
    private const int HostBuild = 34;

    public Task LaunchAsync(IReadOnlyList<string> arguments, CancellationToken cancellationToken)
    {
        var context = global::Android.App.Application.Context;
        var intent = new Intent(Intent.ActionMain);
        intent.SetComponent(new ComponentName(HostPackage, HostActivity));
        intent.PutExtra(ArgumentsExtra, arguments.ToArray());
        intent.AddFlags(ActivityFlags.NewTask | ActivityFlags.ClearTop);
        context.StartActivity(intent);
        return Task.CompletedTask;
    }

    /// <summary>
    /// Creates the launcher instance that stands for the game installed in the host.
    /// </summary>
    internal static void EnsureHostInstance()
    {
        var root = Path.Combine(LauncherUtilities.GetDefaultAppDir(), "Instances", HostInstanceId);
        var meta = Path.Combine(root, "Meta.json");
        if (File.Exists(meta))
            return;

        Directory.CreateDirectory(Path.Combine(root, "Client"));
        Directory.CreateDirectory(Path.Combine(root, "UserData"));
        File.WriteAllText(Path.Combine(root, "Client", "HytaleClient"), "Installed in the Hyprism game host");
        File.WriteAllText(meta, JsonSerializer.Serialize(new
        {
            Id = HostInstanceId,
            Name = "Hytale",
            Branch = "release",
            Version = HostBuild,
            InstalledVersion = HostBuild,
            CreatedAt = DateTime.UtcNow
        }));
    }
}
