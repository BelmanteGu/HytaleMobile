// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

namespace Hyprism.Core.Game.Launch;

/// <summary>
/// Runs the game client in a separate host application instead of a child process.
/// Android registers one to hand authenticated launches to the native game host.
/// </summary>
public interface IExternalGameHost
{
    /// <summary>
    /// Starts the client with the prepared game arguments.
    /// </summary>
    /// <param name="arguments">Client arguments such as the player name, UUID and session tokens.</param>
    /// <param name="cancellationToken">Cancels the launch request.</param>
    /// <returns>A task that completes once the host has been asked to start.</returns>
    Task LaunchAsync(IReadOnlyList<string> arguments, CancellationToken cancellationToken);
}
