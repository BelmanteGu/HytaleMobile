// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

using System.Text.Json;
using Hyprism.Core.Infrastructure;

namespace Hyprism.Core.Accounts;

/// <summary>
/// Provides static helpers for persisting and loading <see cref="HytaleAuthSession"/> data
/// to/from profile-scoped or legacy root-level JSON files.
/// </summary>
internal static class TokenStore
{
    private const string SessionFileName = "HytaleSession.json";
    private const string LegacySessionFileName = "hytale_session.json";

    /// <summary>
    /// Resolves the canonical session file path for the current profile.
    /// Falls back to app root when no profile folder is available.
    /// </summary>
    /// <param name="profileFolder">
    /// The resolved path of the active profile folder, or <c>null</c> if no profile is active.
    /// </param>
    /// <param name="appDir">The launcher application data directory.</param>
    /// <returns>Absolute path to the session JSON file.</returns>
    public static string GetSessionFilePath(string? profileFolder, string appDir)
    {
        if (profileFolder != null)
        {
            Directory.CreateDirectory(profileFolder);
            return LauncherJsonFile.GetPath(profileFolder, SessionFileName, LegacySessionFileName);
        }
        return GetLegacySessionFilePath(appDir);
    }

    /// <summary>
    /// Gets the legacy (pre-profile) session file path at the app root.
    /// </summary>
    /// <returns>The requested legacy session file path</returns>
    public static string GetLegacySessionFilePath(string appDir) =>
        LauncherJsonFile.GetPath(appDir, SessionFileName, LegacySessionFileName);

    /// <summary>
    /// Loads a <see cref="HytaleAuthSession"/> from the specified file path.
    /// Returns <c>null</c> when the file does not exist or deserialisation fails.
    /// </summary>
    /// <returns>The loaded value, or null when unavailable</returns>
    public static HytaleAuthSession? Load(string filePath)
    {
        if (!File.Exists(filePath)) return null;
        try
        {
            var json = File.ReadAllText(filePath);
            return JsonSerializer.Deserialize<HytaleAuthSession>(json, JsonDefaults.CaseInsensitive);
        }
        catch (Exception ex)
        {
            Logger.Warning("TokenStore", $"Failed to load session from '{filePath}': {ex.Message}");
            return null;
        }
    }

    /// <summary>
    /// Persists a <see cref="HytaleAuthSession"/> to the specified file path.
    /// </summary>
    public static void Save(string filePath, HytaleAuthSession session)
    {
        try
        {
            Write(filePath, session);
        }
        catch (Exception ex)
        {
            Logger.Warning("TokenStore", $"Failed to save session to '{filePath}': {ex.Message}");
        }
    }

    /// <summary>
    /// Writes the session through a temporary file and a rename, so a process killed mid-write (Android
    /// stops background apps without notice) leaves the previous session instead of a truncated file.
    /// </summary>
    /// <exception cref="IOException">The file could not be written.</exception>
    public static void Write(string filePath, HytaleAuthSession session)
    {
        var json = JsonSerializer.Serialize(session, JsonDefaults.Indented);
        var tempPath = filePath + ".tmp";
        using (var stream = new FileStream(tempPath, FileMode.Create, FileAccess.Write, FileShare.None))
        using (var writer = new StreamWriter(stream))
        {
            writer.Write(json);
            writer.Flush();
            stream.Flush(flushToDisk: true);
        }
        File.Move(tempPath, filePath, overwrite: true);
    }

    /// <summary>
    /// Deletes the session file at the specified path (best-effort, ignores errors).
    /// </summary>
    public static void Delete(string filePath)
    {
        try
        {
            if (File.Exists(filePath))
                File.Delete(filePath);
        }
        catch (Exception ex)
        {
            Logger.Warning("TokenStore", $"Failed to delete session file '{filePath}': {ex.Message}");
        }
    }
}
