// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

using Android.App;
using Android.Runtime;
using Avalonia;
using Avalonia.Android;
using Hyprism.Core.Game.Launch;
using Microsoft.Extensions.DependencyInjection;

namespace Hyprism.Desktop.Android;

[Application]
public sealed class MainApplication : AvaloniaAndroidApplication<App>
{
    private MainApplication(nint javaReference, JniHandleOwnership transfer)
        : base(javaReference, transfer)
    {
    }

    protected override AppBuilder CustomizeAppBuilder(AppBuilder builder)
    {
        // Android truncates managed stack traces; log every unhandled exception in full.
        AndroidEnvironment.UnhandledExceptionRaiser += (_, args) =>
            global::Android.Util.Log.Error("Hyprism", $"Unhandled exception: {args.Exception}");
        AppDomain.CurrentDomain.UnhandledException += (_, args) =>
            global::Android.Util.Log.Error("Hyprism", $"Unhandled exception: {args.ExceptionObject}");
        // The launcher log goes to Console.Out, which Android drops; send it to logcat (tag Hyprism) so a
        // failed login or token refresh can be read with adb logcat -s Hyprism. LauncherStartup captures it.
        Console.SetOut(new LogcatWriter());
        Hyprism.Desktop.Platform.PlatformActions.PregenerateWorld = () =>
        {
            var context = global::Android.App.Application.Context;
            var intent = new global::Android.Content.Intent();
            intent.SetComponent(new global::Android.Content.ComponentName(
                AndroidGameHost.HostPackage, "org.hyprism.game.PregenerateActivity"));
            intent.AddFlags(global::Android.Content.ActivityFlags.NewTask);
            context.StartActivity(intent);
        };
        AndroidGameHost.EnsureHostInstance();
        LauncherStartup.Initialize(services =>
        {
            services.AddSingleton<ILocalNodeServiceFactory, UnsupportedLocalNodeServiceFactory>();
            services.AddSingleton<IExternalGameHost, AndroidGameHost>();
        });
        return base.CustomizeAppBuilder(builder);
    }
}

/// <summary>
/// A console that writes each line to logcat.
/// </summary>
internal sealed class LogcatWriter : System.IO.TextWriter
{
    private readonly System.Text.StringBuilder _line = new();

    public override System.Text.Encoding Encoding => System.Text.Encoding.UTF8;

    public override void Write(char value)
    {
        lock (_line)
            Append(value);
    }

    public override void Write(string? value)
    {
        if (value == null)
            return;
        lock (_line)
        {
            foreach (var c in value)
                Append(c);
        }
    }

    private void Append(char c)
    {
        if (c == '\n')
        {
            global::Android.Util.Log.Info("Hyprism", _line.ToString());
            _line.Clear();
        }
        else if (c != '\r')
        {
            _line.Append(c);
        }
    }
}

/// <summary>
/// Android has no ASP.NET Core host, so the Local Node is unavailable on this head.
/// </summary>
internal sealed class UnsupportedLocalNodeServiceFactory : ILocalNodeServiceFactory
{
    public ILocalNodeService Create()
        => throw new PlatformNotSupportedException("The Local Node is not available on Android");
}
