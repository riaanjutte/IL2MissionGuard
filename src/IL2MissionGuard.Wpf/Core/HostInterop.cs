using System.ComponentModel;
using System.Diagnostics;

namespace IL2MissionGuard.Core;

internal static class HostInterop
{
    private static string hostPath = string.Empty;

    public static void Initialize(IReadOnlyList<string> arguments)
    {
        for (int index = 0; index + 1 < arguments.Count; index++)
        {
            if (arguments[index].Equals("--host", StringComparison.OrdinalIgnoreCase))
            {
                hostPath = arguments[index + 1];
                break;
            }
        }
    }

    public static void RequestUpdateCheck()
    {
        if (string.IsNullOrWhiteSpace(hostPath) || !File.Exists(hostPath))
        {
            throw new InvalidOperationException("The lightweight Mission Guard tray host could not be located.");
        }

        try
        {
            _ = Process.Start(new ProcessStartInfo(hostPath, "--check-updates") { UseShellExecute = true });
        }
        catch (Win32Exception error)
        {
            throw new InvalidOperationException("Windows could not ask the Mission Guard tray host to install the update.", error);
        }
    }
}
