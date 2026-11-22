// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

using Hyprism.Core;
using Hyprism.Core.Accounts;
using Hyprism.Core.Application.Ports;
using Hyprism.Core.Infrastructure;
using Hyprism.Desktop.Integrations.Discord;
using Hyprism.Desktop.Integrations.GitHub;
using Hyprism.Desktop.Platform;
using Hyprism.Desktop.Screens.News;
using Hyprism.Desktop.Screens.Settings;
using Microsoft.Extensions.DependencyInjection;

namespace Hyprism.Desktop;

/// <summary>
/// Builds the service provider shared by every application head.
/// Each head adds the services that only exist on its platform.
/// </summary>
internal static class LauncherStartup
{
    public static void Initialize(Action<IServiceCollection> addPlatformServices)
    {
        Logger.CaptureOriginalConsole();
        LauncherUserAgent.ConfigureVersion(DesktopApplicationInfo.Version);
        DesktopRuntime.Services = Bootstrapper.Initialize(services =>
        {
            services.AddSingleton<DesktopSettingsStore>();
            services.AddSingleton<IDesktopSettingsStore>(provider =>
                provider.GetRequiredService<DesktopSettingsStore>());
            services.AddSingleton<HytaleNewsClient>();
            services.AddSingleton<IHytaleNewsClient>(provider =>
                provider.GetRequiredService<HytaleNewsClient>());
            services.AddSingleton<GitHubClient>();
            services.AddSingleton<IGitHubClient>(provider =>
                provider.GetRequiredService<GitHubClient>());
            services.AddSingleton<RemoteImageCache>();
            services.AddSingleton<DiscordPresence>();
            services.AddSingleton<IDiscordPresence>(provider =>
                provider.GetRequiredService<DiscordPresence>());
            services.AddSingleton<GpuProvider>();
            services.AddSingleton<IGpuProvider>(provider =>
                provider.GetRequiredService<GpuProvider>());
            services.AddSingleton<IOAuthCallbackPageRenderer>(
                provider => new OAuthCallbackPageRenderer(
                    provider.GetRequiredService<IDesktopSettingsStore>()));
            addPlatformServices(services);
        });
    }
}

internal static class DesktopRuntime
{
    public static IServiceProvider Services { get; set; } = null!;
}
