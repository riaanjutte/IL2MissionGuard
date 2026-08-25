using System.ComponentModel;
using System.Diagnostics;
using Microsoft.Win32;

namespace IL2MissionGuard.Core;

internal static class EditorInterop
{
    private const string GreatBattlesProcess = "STEditor";
    private const string KoreaProcess = "IL2Editor";

    public static List<EditorProcess> FindEditors(AutoSaveOptions options)
    {
        List<EditorProcess> editors = [];
        if (options.GreatBattles)
        {
            AddProcesses(editors, GreatBattlesProcess);
        }

        if (options.Korea)
        {
            AddProcesses(editors, KoreaProcess);
        }

        return editors;
    }

    public static bool TryGetSavedMissionPath(string title, out string missionPath, bool requireExists = true)
    {
        missionPath = string.Empty;
        int separator = title.IndexOf(" - ", StringComparison.Ordinal);
        if (separator < 0)
        {
            return false;
        }

        string candidate = title[(separator + 3)..].Trim();
        if (candidate.Length == 0 || candidate.Contains("<empty>", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        if (candidate.EndsWith('*'))
        {
            candidate = candidate[..^1].TrimEnd();
        }

        if (!Path.IsPathFullyQualified(candidate))
        {
            return false;
        }

        if (Path.GetExtension(candidate).Equals(".msnbin", StringComparison.OrdinalIgnoreCase))
        {
            candidate = Path.ChangeExtension(candidate, ".Mission");
        }
        else if (!Path.GetExtension(candidate).Equals(".Mission", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        missionPath = Path.GetFullPath(candidate);
        return !requireExists || File.Exists(missionPath);
    }

    public static bool SendSave(EditorProcess editor)
    {
        if (editor.WindowHandle == 0 || !NativeMethods.IsWindowEnabled(editor.WindowHandle))
        {
            return false;
        }

        return NativeMethods.SendMessageTimeout(
            editor.WindowHandle,
            NativeMethods.WmCommand,
            NativeMethods.SaveCommand,
            0,
            NativeMethods.SmtoBlock | NativeMethods.SmtoAbortIfHung,
            10_000,
            out _) != 0;
    }

    public static async Task CloseEditorAsync(EditorProcess editor, CancellationToken cancellationToken)
    {
        using Process? process = TryGetProcess(editor.Id);
        if (process is null)
        {
            return;
        }

        if (!process.CloseMainWindow())
        {
            throw new InvalidOperationException("The Mission Editor did not accept the close request. Close it manually and try again.");
        }

        using CancellationTokenSource timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(TimeSpan.FromMinutes(2));
        try
        {
            await process.WaitForExitAsync(timeout.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            throw new TimeoutException("The Mission Editor did not close within two minutes. No files were restored.");
        }
    }

    public static async Task<(bool Reopened, string Diagnostic)> OpenMissionAsync(
        string executablePath,
        string processName,
        string missionPath,
        CancellationToken cancellationToken)
    {
        if (!File.Exists(executablePath) || !File.Exists(missionPath))
        {
            return (false, "The editor executable or restored mission was unavailable.");
        }

        string? keyPath = FindOptionsKey(processName, missionPath);
        if (keyPath is null)
        {
            return (false, "The Mission Editor registry profile could not be identified safely.");
        }

        using RegistryKey key = Registry.CurrentUser.CreateSubKey(keyPath, true)
            ?? throw new InvalidOperationException("The Mission Editor startup settings could not be opened.");
        RegistryBackup[] backups =
        [
            RegistryBackup.Capture(key, "OpenLastUsedMissionOnStart"),
            RegistryBackup.Capture(key, "LastUsedMissionName"),
            RegistryBackup.Capture(key, "LastUsedMissionFolder"),
        ];

        void RestoreSettings()
        {
            foreach (RegistryBackup backup in backups)
            {
                backup.Restore(key);
            }

            key.Flush();
        }

        try
        {
            string startupMission = missionPath;
            string binary = Path.ChangeExtension(missionPath, ".msnbin");
            if (processName.Equals(KoreaProcess, StringComparison.OrdinalIgnoreCase) && File.Exists(binary))
            {
                startupMission = binary;
            }

            key.SetValue("OpenLastUsedMissionOnStart", 1, RegistryValueKind.DWord);
            key.SetValue("LastUsedMissionName", startupMission, RegistryValueKind.String);
            key.SetValue("LastUsedMissionFolder", Path.TrimEndingDirectorySeparator(Path.GetDirectoryName(missionPath)!) + Path.DirectorySeparatorChar, RegistryValueKind.String);
            key.Flush();

            using Process process = Process.Start(new ProcessStartInfo(executablePath)
            {
                WorkingDirectory = Path.GetDirectoryName(executablePath),
                UseShellExecute = false,
            }) ?? throw new InvalidOperationException("The Mission Editor process could not be started.");

            DateTime deadline = DateTime.UtcNow.AddMinutes(2);
            while (DateTime.UtcNow < deadline)
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (process.HasExited)
                {
                    RestoreSettings();
                    return (false, "The Mission Editor exited before the restored mission finished opening.");
                }

                process.Refresh();
                if (process.MainWindowHandle != 0 &&
                    TryGetSavedMissionPath(process.MainWindowTitle, out string opened, false) &&
                    PathEquals(opened, missionPath))
                {
                    RestoreSettings();
                    return (true, string.Empty);
                }

                await Task.Delay(200, cancellationToken).ConfigureAwait(false);
            }

            RestoreSettings();
            return (false, "Timed out waiting for the restored mission to open.");
        }
        catch
        {
            RestoreSettings();
            throw;
        }
    }

    public static EditorProcess? FindEditorForSnapshot(Snapshot snapshot, AutoSaveOptions options) =>
        FindEditors(options).FirstOrDefault(editor =>
            editor.Name.Equals(snapshot.EditorProcessName, StringComparison.OrdinalIgnoreCase) &&
            TryGetSavedMissionPath(editor.Title, out string mission, false) &&
            PathEquals(mission, snapshot.MissionPath));

    private static void AddProcesses(List<EditorProcess> result, string name)
    {
        foreach (Process process in Process.GetProcessesByName(name))
        {
            using (process)
            {
                try
                {
                    process.Refresh();
                    if (process.MainWindowHandle == 0)
                    {
                        continue;
                    }

                    string executable = string.Empty;
                    try
                    {
                        executable = process.MainModule?.FileName ?? string.Empty;
                    }
                    catch (Exception error) when (error is Win32Exception or InvalidOperationException)
                    {
                        AppLog.Write($"Could not query {name} executable path: {error.Message}");
                    }

                    result.Add(new EditorProcess(process.Id, name, executable, process.MainWindowHandle, process.MainWindowTitle));
                }
                catch (Exception error) when (error is InvalidOperationException or Win32Exception)
                {
                    AppLog.Write($"Could not inspect {name}: {error.Message}");
                }
            }
        }
    }

    private static Process? TryGetProcess(int id)
    {
        try
        {
            return Process.GetProcessById(id);
        }
        catch (ArgumentException)
        {
            return null;
        }
    }

    private static string? FindOptionsKey(string processName, string missionPath)
    {
        if (processName.Equals(GreatBattlesProcess, StringComparison.OrdinalIgnoreCase))
        {
            return @"Software\1CGS_IL2\STEditor\EditorOptions";
        }

        if (!processName.Equals(KoreaProcess, StringComparison.OrdinalIgnoreCase))
        {
            return null;
        }

        using RegistryKey? root = Registry.CurrentUser.OpenSubKey(@"Software\1CGS");
        if (root is null)
        {
            return null;
        }

        List<string> candidates = [];
        foreach (string name in root.GetSubKeyNames().Where(name => name.StartsWith("STEditor_", StringComparison.OrdinalIgnoreCase)))
        {
            string relative = @"Software\1CGS\" + name + @"\EditorOptions";
            using RegistryKey? options = Registry.CurrentUser.OpenSubKey(relative);
            if (options is null)
            {
                continue;
            }

            candidates.Add(relative);
            if (options.GetValue("LastUsedMissionName") is string last && SameMission(last, missionPath))
            {
                return relative;
            }
        }

        return candidates.Count == 1 ? candidates[0] : null;
    }

    private static bool SameMission(string left, string right)
    {
        try
        {
            return PathEquals(CanonicalMission(left), CanonicalMission(right));
        }
        catch (ArgumentException)
        {
            return false;
        }
    }

    private static string CanonicalMission(string path)
    {
        string result = Path.GetFullPath(path);
        if (Path.GetExtension(result).Equals(".msnbin", StringComparison.OrdinalIgnoreCase))
        {
            result = Path.ChangeExtension(result, ".Mission");
        }

        if (!Path.GetExtension(result).Equals(".Mission", StringComparison.OrdinalIgnoreCase))
        {
            throw new ArgumentException("Not a mission path.", nameof(path));
        }

        return result;
    }

    private static bool PathEquals(string left, string right) =>
        Path.GetFullPath(left).Equals(Path.GetFullPath(right), StringComparison.OrdinalIgnoreCase);

    private sealed class RegistryBackup
    {
        private RegistryBackup(string name, bool existed, object? value, RegistryValueKind kind)
        {
            Name = name;
            Existed = existed;
            Value = value;
            Kind = kind;
        }

        private string Name { get; }

        private bool Existed { get; }

        private object? Value { get; }

        private RegistryValueKind Kind { get; }

        public static RegistryBackup Capture(RegistryKey key, string name)
        {
            object? value = key.GetValue(name, null, RegistryValueOptions.DoNotExpandEnvironmentNames);
            return value is null
                ? new RegistryBackup(name, false, null, RegistryValueKind.None)
                : new RegistryBackup(name, true, value, key.GetValueKind(name));
        }

        public void Restore(RegistryKey key)
        {
            if (Existed)
            {
                key.SetValue(Name, Value!, Kind);
            }
            else
            {
                key.DeleteValue(Name, false);
            }
        }
    }
}
