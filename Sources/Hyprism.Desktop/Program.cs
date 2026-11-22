// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

using Avalonia;
using Avalonia.Skia;
using Avalonia.X11;
using Hyprism.Desktop.Platform;
using Hyprism.Core.Game.Launch;
using Hyprism.LocalNode;
using Microsoft.Extensions.DependencyInjection;

namespace Hyprism.Desktop;

internal static class Program
{
    [STAThread]
    public static void Main(string[] args)
    {
        LauncherStartup.Initialize(services =>
        {
            services.AddSingleton<LocalNodeServiceFactory>();
            services.AddSingleton<ILocalNodeServiceFactory>(provider =>
                provider.GetRequiredService<LocalNodeServiceFactory>());
        });

        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }

    public static AppBuilder BuildAvaloniaApp()
        => AppBuilder.Configure<App>()
            .UsePlatformDetect()
#pragma warning disable AVALONIA_X11_CSD
            .With(new X11PlatformOptions
            {
                EnableDrawnDecorations = true
            })
#pragma warning restore AVALONIA_X11_CSD
            .With(DesktopRenderOptions.CreateWin32Options())
            .With(new SkiaOptions
            {
                MaxGpuResourceSizeBytes = 256 * 1024 * 1024
            });
}
