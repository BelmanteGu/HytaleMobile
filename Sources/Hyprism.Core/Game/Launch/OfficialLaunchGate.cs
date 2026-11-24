// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

using Hyprism.Core.Infrastructure;

namespace Hyprism.Core.Game.Launch;

/// <summary>
/// Serializes official authentication and process registration across launcher processes.
/// The lock is short lived: it prevents a concurrent launch from missing a just-created
/// registry entry, while the persistent process registry protects the running lifetime.
/// </summary>
/// <remarks>
/// The lock is an exclusively opened file, not a named <see cref="Mutex"/>: the launch awaits network calls
/// while it holds the lock, so it is released on whichever thread the continuation runs on, and a mutex may
/// only be released by the thread that took it (on Android that threw SynchronizationLockException and
/// failed every official launch). The operating system drops the file lock if the process dies.
/// </remarks>
internal sealed class OfficialLaunchGate : IDisposable
{
    private static readonly TimeSpan Timeout = TimeSpan.FromMinutes(2);
    private readonly string _path;
    private FileStream? _lock;

    public OfficialLaunchGate()
        : this(Path.Combine(LauncherUtilities.GetDefaultAppDir(), "official-launch.lock"))
    {
    }

    internal OfficialLaunchGate(string path)
    {
        _path = path;
    }

    public void Enter()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
        var deadline = DateTime.UtcNow + Timeout;
        while (true)
        {
            try
            {
                _lock = new FileStream(_path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
                return;
            }
            catch (IOException) when (DateTime.UtcNow < deadline)
            {
                Thread.Sleep(100);
            }
            catch (IOException ex)
            {
                throw new TimeoutException(
                    "Timed out waiting for another official Hytale launch to finish authentication", ex);
            }
        }
    }

    public void Dispose()
    {
        _lock?.Dispose();
        _lock = null;
    }
}
