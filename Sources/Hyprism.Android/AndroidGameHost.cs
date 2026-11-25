// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

using System.Text.Json;
using Android.Content;
using Hyprism.Core.Game.Launch;
using Hyprism.Core.Infrastructure;

namespace Hyprism.Desktop.Android;

/// <summary>
/// Hands authenticated launches to the game half of this app, which runs the macOS arm64 client
/// through the Hyprism Mach-O loader in its own process.
/// </summary>
internal sealed class AndroidGameHost : IExternalGameHost
{
    // SetupActivity prepares the world server, then opens HytaleActivity with these arguments.
    internal const string SetupActivity = "org.hyprism.game.SetupActivity";
    internal const string ArgumentsExtra = "org.hyprism.game.ARGUMENTS";

    // The one instance on Android: the game reads its files from the app's external files folder.
    private const string HostInstanceId = "6f8d1c2a-4b7e-4a3f-9c55-0a1b2c3d4e5f";
    // The build the loader is tested with.
    private const int HostBuild = 34;

    public Task LaunchAsync(IReadOnlyList<string> arguments, CancellationToken cancellationToken)
    {
        var context = global::Android.App.Application.Context;
        var intent = new Intent(Intent.ActionMain);
        intent.SetComponent(new ComponentName(context.PackageName!, SetupActivity));
        intent.PutExtra(ArgumentsExtra, arguments.ToArray());
        intent.AddFlags(ActivityFlags.NewTask | ActivityFlags.ClearTop);
        context.StartActivity(intent);
        return Task.CompletedTask;
    }

    /// <summary>
    /// Makes the launcher's instance folder the game's files folder (a link to it), so the launcher
    /// downloads and updates the macOS build right where the game reads it:
    /// Client/Hytale.app, Server/ and Assets.zip, next to the game's UserData.
    /// </summary>
    internal static void EnsureHostInstance()
    {
        var context = global::Android.App.Application.Context;
        var gameFiles = context.GetExternalFilesDir(null)?.AbsolutePath;
        if (gameFiles is null)
        {
            Logger.Warning("Android", "No external files folder; the game cannot be installed");
            return;
        }
        Directory.CreateDirectory(gameFiles);

        var instances = Path.Combine(LauncherUtilities.GetDefaultAppDir(), "Instances");
        Directory.CreateDirectory(instances);
        var root = Path.Combine(instances, HostInstanceId);
        var link = new FileInfo(root);
        if (link.LinkTarget != gameFiles)
        {
            // A folder from an older launcher held only a marker file and Meta.json.
            if (Directory.Exists(root) && link.LinkTarget is null)
                Directory.Delete(root, recursive: true);
            else if (link.LinkTarget is not null)
                File.Delete(root);
            Directory.CreateSymbolicLink(root, gameFiles);
            Logger.Info("Android", $"Instance {HostInstanceId} is the game folder {gameFiles}");
        }

        var meta = Path.Combine(root, "Meta.json");
        var installed = File.Exists(Path.Combine(root, "Client", "Hytale.app", "Contents", "MacOS", "HytaleClient"));
        if (File.Exists(meta))
            return;
        Directory.CreateDirectory(Path.Combine(root, "UserData"));
        File.WriteAllText(meta, JsonSerializer.Serialize(new
        {
            Id = HostInstanceId,
            Name = "Hytale",
            Branch = "release",
            Version = HostBuild,
            InstalledVersion = installed ? HostBuild : 0,
            CreatedAt = DateTime.UtcNow
        }));
    }
}
