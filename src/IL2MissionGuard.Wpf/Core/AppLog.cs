using System.Text;

namespace IL2MissionGuard.Core;

internal static class AppLog
{
    private static readonly Lock Sync = new();

    public static void Write(string message)
    {
        try
        {
            Directory.CreateDirectory(SettingsStore.LocalAppDataDirectory);
            lock (Sync)
            {
                File.AppendAllText(
                    SettingsStore.DefaultLogPath,
                    $"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}  {message}{Environment.NewLine}",
                    new UTF8Encoding(false));
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            System.Diagnostics.Debug.WriteLine(error);
        }
    }
}
