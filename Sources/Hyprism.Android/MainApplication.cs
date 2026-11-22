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
/// Android has no ASP.NET Core host, so the Local Node is unavailable on this head.
/// </summary>
internal sealed class UnsupportedLocalNodeServiceFactory : ILocalNodeServiceFactory
{
    public ILocalNodeService Create()
        => throw new PlatformNotSupportedException("The Local Node is not available on Android");
}
