using System.Runtime.InteropServices;
using System.Text;

namespace IL2MissionGuard.Core;

internal static class SettingsStore
{
    private const string Section = "AutoSave";

    public static string LocalAppDataDirectory => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "IL2MEC");

    public static string DefaultSettingsPath
    {
        get
        {
            string? configured = Environment.GetEnvironmentVariable("IL2MISSIONGUARD_SETTINGS_FILE");
            configured = string.IsNullOrWhiteSpace(configured) ? Environment.GetEnvironmentVariable("IL2MEC_SETTINGS_FILE") : configured;
            return string.IsNullOrWhiteSpace(configured) ? Path.Combine(LocalAppDataDirectory, "IL2MEC.ini") : Path.GetFullPath(configured);
        }
    }

    public static string DefaultSnapshotRoot => Path.Combine(LocalAppDataDirectory, "Autosave");

    public static string DefaultLogPath => Path.Combine(LocalAppDataDirectory, "autosave.log");

    public static string LegacySnapshotRoot => Path.GetFullPath(Path.Combine(Path.GetTempPath(), "STEditor", "Autosave"));

    public static AutoSaveOptions Load(string path)
    {
        if (!File.Exists(path))
        {
            return new AutoSaveOptions();
        }

        bool ReadBoolean(string key, bool fallback)
        {
            string value = Read(path, key, fallback ? "true" : "false");
            return value.Equals("true", StringComparison.OrdinalIgnoreCase) || value == "1";
        }

        int ReadInteger(string key, int fallback) => int.TryParse(Read(path, key, fallback.ToString()), out int value) ? value : fallback;

        string themeValue = Read(path, "Theme", "System");
        ThemeMode theme = themeValue.Equals("Dark", StringComparison.OrdinalIgnoreCase)
            ? ThemeMode.Dark
            : themeValue.Equals("Light", StringComparison.OrdinalIgnoreCase) ? ThemeMode.Light : ThemeMode.System;

        AutoSaveOptions options = new(
            Enabled: ReadBoolean("Enabled", true),
            GreatBattles: ReadBoolean("GreatBattles", true),
            Korea: ReadBoolean("Korea", true),
            IntervalMinutes: ReadInteger("IntervalMinutes", 5),
            HistoricSnapshots: ReadInteger("HistoricSnapshots", 10),
            TrayNotifications: ReadBoolean("TrayNotifications", true),
            Theme: theme);

        return IsValid(options) ? options : new AutoSaveOptions();
    }

    public static void Save(string path, AutoSaveOptions options)
    {
        if (!IsValid(options))
        {
            throw new ArgumentOutOfRangeException(nameof(options), "The autosave settings are outside the supported range.");
        }

        string fullPath = Path.GetFullPath(path);
        Directory.CreateDirectory(Path.GetDirectoryName(fullPath)!);
        bool existed = File.Exists(fullPath);
        byte[]? original = existed ? File.ReadAllBytes(fullPath) : null;

        try
        {
            Write(fullPath, "Enabled", options.Enabled ? "true" : "false");
            Write(fullPath, "GreatBattles", options.GreatBattles ? "true" : "false");
            Write(fullPath, "Korea", options.Korea ? "true" : "false");
            Write(fullPath, "IntervalMinutes", options.IntervalMinutes.ToString());
            Write(fullPath, "HistoricSnapshots", options.HistoricSnapshots.ToString());
            Write(fullPath, "TrayNotifications", options.TrayNotifications ? "true" : "false");
            Write(fullPath, "Theme", options.Theme.ToString());
            _ = WritePrivateProfileString(null, null, null, fullPath);
        }
        catch
        {
            if (original is not null)
            {
                File.WriteAllBytes(fullPath, original);
            }
            else
            {
                File.Delete(fullPath);
            }

            _ = WritePrivateProfileString(null, null, null, fullPath);
            throw;
        }
    }

    private static bool IsValid(AutoSaveOptions options) =>
        options.IntervalMinutes is >= 1 and <= 60 &&
        options.HistoricSnapshots is >= 1 and <= 100 &&
        (!options.Enabled || options.GreatBattles || options.Korea);

    private static string Read(string path, string key, string fallback)
    {
        StringBuilder buffer = new(128);
        _ = GetPrivateProfileString(Section, key, fallback, buffer, buffer.Capacity, path);
        return buffer.ToString().Trim();
    }

    private static void Write(string path, string key, string value)
    {
        if (!WritePrivateProfileString(Section, key, value, path))
        {
            throw new InvalidOperationException("Windows could not update the autosave settings file.");
        }
    }

    [DllImport("kernel32.dll", EntryPoint = "GetPrivateProfileStringW", CharSet = CharSet.Unicode)]
    private static extern uint GetPrivateProfileString(
        string section,
        string key,
        string defaultValue,
        StringBuilder returnedString,
        int size,
        string filePath);

    [DllImport("kernel32.dll", EntryPoint = "WritePrivateProfileStringW", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool WritePrivateProfileString(string? section, string? key, string? value, string filePath);
}
