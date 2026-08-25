using System.Drawing;
using System.Windows;
using IL2MissionGuard.Core;
using Wpf.Ui.Appearance;
using Wpf.Ui.Controls;
using Forms = System.Windows.Forms;

namespace IL2MissionGuard;

public partial class App : System.Windows.Application
{
    private const string MutexName = @"Local\IL2MEC.AutoSave.Agent";
    private const string ShowEventName = @"Local\IL2MissionGuard.Show";
    private const string SettingsEventName = @"Local\IL2MissionGuard.Settings";
    private const string UpdateEventName = @"Local\IL2MissionGuard.CheckUpdates";
    private Mutex? instanceMutex;
    private EventWaitHandle? showEvent;
    private EventWaitHandle? settingsEvent;
    private EventWaitHandle? updateEvent;
    private CancellationTokenSource? commandCancellation;
    private MissionGuardService? service;
    private MainWindow? mainWindow;
    private Forms.NotifyIcon? trayIcon;

    internal bool IsShuttingDown { get; private set; }

    protected override async void OnStartup(StartupEventArgs eventArgs)
    {
        base.OnStartup(eventArgs);
        if (eventArgs.Args.Length == 3 && eventArgs.Args[0].Equals("--apply-update", StringComparison.OrdinalIgnoreCase))
        {
            try
            {
                int processId = int.Parse(eventArgs.Args[2], System.Globalization.CultureInfo.InvariantCulture);
                Environment.ExitCode = await UpdateService.ApplyPendingUpdateAsync(eventArgs.Args[1], processId);
            }
            catch (Exception error)
            {
                System.Windows.MessageBox.Show(error.Message, "IL-2 Mission Guard update", System.Windows.MessageBoxButton.OK, System.Windows.MessageBoxImage.Error);
                Environment.ExitCode = 1;
            }

            Shutdown(Environment.ExitCode);
            return;
        }

        bool ownsMutex;
        instanceMutex = new Mutex(true, MutexName, out ownsMutex);
        if (!ownsMutex)
        {
            SignalExistingInstance(eventArgs.Args);
            instanceMutex.Dispose();
            instanceMutex = null;
            Shutdown();
            return;
        }

        try
        {
            AutoSaveOptions options = SettingsStore.Load(SettingsStore.DefaultSettingsPath);
            ApplyTheme(options.Theme);
            service = new MissionGuardService();
            mainWindow = new MainWindow(service);
            MainWindow = mainWindow;
            CreateTrayIcon();
            service.NotificationRaised += Service_NotificationRaised;
            service.StatusChanged += Service_StatusChanged;
            service.Start();
            StartCommandListener();

            if (eventArgs.Args.Contains("--settings", StringComparer.OrdinalIgnoreCase))
            {
                mainWindow.ShowSettings();
            }
            else if (eventArgs.Args.Contains("--status", StringComparer.OrdinalIgnoreCase))
            {
                mainWindow.ShowOverview();
            }

            if (eventArgs.Args.Contains("--check-updates", StringComparer.OrdinalIgnoreCase))
            {
                _ = mainWindow.CheckUpdatesAsync(true);
            }
            else
            {
                _ = CheckForUpdatesAfterStartupAsync();
            }
        }
        catch (Exception error)
        {
            AppLog.Write("Startup failed: " + error);
            System.Windows.MessageBox.Show(error.Message, "IL-2 Mission Guard", System.Windows.MessageBoxButton.OK, System.Windows.MessageBoxImage.Error);
            ExitApplication();
        }
    }

    internal void ShowTrayNotification(string title, string message, bool error)
    {
        trayIcon?.ShowBalloonTip(7000, title, message, error ? Forms.ToolTipIcon.Error : Forms.ToolTipIcon.Info);
    }

    internal void ExitApplication()
    {
        IsShuttingDown = true;
        trayIcon?.Dispose();
        trayIcon = null;
        commandCancellation?.Cancel();
        service?.Dispose();
        service = null;
        instanceMutex?.ReleaseMutex();
        instanceMutex?.Dispose();
        instanceMutex = null;
        Shutdown();
    }

    protected override void OnExit(ExitEventArgs eventArgs)
    {
        trayIcon?.Dispose();
        commandCancellation?.Cancel();
        showEvent?.Dispose();
        settingsEvent?.Dispose();
        updateEvent?.Dispose();
        service?.Dispose();
        if (instanceMutex is not null)
        {
            try
            {
                instanceMutex.ReleaseMutex();
            }
            catch (ApplicationException)
            {
            }

            instanceMutex.Dispose();
        }

        base.OnExit(eventArgs);
    }

    private void CreateTrayIcon()
    {
        string executable = Environment.ProcessPath ?? throw new InvalidOperationException("Windows could not locate Mission Guard.");
        Icon icon = Icon.ExtractAssociatedIcon(executable) ?? SystemIcons.Shield;
        Forms.ContextMenuStrip menu = new();
        menu.Items.Add("Open Mission Guard", null, (_, _) => mainWindow?.ShowOverview());
        menu.Items.Add(new Forms.ToolStripSeparator());
        menu.Items.Add("Create recovery point now", null, async (_, _) => await CreateRecoveryFromTrayAsync());
        menu.Items.Add("Browse recovery points", null, (_, _) =>
        {
            mainWindow?.ShowOverview();
            mainWindow?.ShowRecoveryPage();
        });
        menu.Items.Add(new Forms.ToolStripSeparator());
        menu.Items.Add("Settings…", null, (_, _) => mainWindow?.ShowSettings());
        menu.Items.Add("Open recovery folder", null, (_, _) => OpenPath(SettingsStore.DefaultSnapshotRoot));
        menu.Items.Add("Open diagnostic log", null, (_, _) => OpenPath(SettingsStore.DefaultLogPath));
        menu.Items.Add("Check for updates…", null, async (_, _) =>
        {
            if (mainWindow is not null)
            {
                await mainWindow.CheckUpdatesAsync(true);
            }
        });
        menu.Items.Add(new Forms.ToolStripSeparator());
        menu.Items.Add("Exit Mission Guard", null, (_, _) => ExitApplication());

        trayIcon = new Forms.NotifyIcon
        {
            Icon = icon,
            Text = "IL-2 Mission Guard",
            Visible = true,
            ContextMenuStrip = menu,
        };
        trayIcon.DoubleClick += (_, _) => mainWindow?.ShowOverview();
    }

    private async Task CreateRecoveryFromTrayAsync()
    {
        if (service is null)
        {
            return;
        }

        IReadOnlyList<SaveAttemptResult> results = await service.CreateRecoveryPointNowAsync();
        SaveAttemptResult? failure = results.FirstOrDefault(result => result.Failure);
        string summary = string.Join(" ", results.Select(result => $"{result.Subject}: {result.Detail}"));
        ShowTrayNotification(failure is null ? "Mission Guard" : "Mission Guard recovery point failed", summary, failure is not null);
    }

    private void Service_NotificationRaised(object? sender, GuardNotification notification) =>
        ShowTrayNotification(notification.Title, notification.Message, notification.IsError);

    private void Service_StatusChanged(object? sender, EventArgs eventArgs)
    {
        if (trayIcon is null || service is null)
        {
            return;
        }

        GuardStatus status = service.Status;
        trayIcon.Text = status.State switch
        {
            ProtectionState.Protected => "IL-2 Mission Guard — protected",
            ProtectionState.Error => "IL-2 Mission Guard — attention needed",
            _ => $"IL-2 Mission Guard — every {service.Options.IntervalMinutes} min",
        };
    }

    private void StartCommandListener()
    {
        showEvent = new EventWaitHandle(false, EventResetMode.AutoReset, ShowEventName);
        settingsEvent = new EventWaitHandle(false, EventResetMode.AutoReset, SettingsEventName);
        updateEvent = new EventWaitHandle(false, EventResetMode.AutoReset, UpdateEventName);
        commandCancellation = new CancellationTokenSource();
        _ = ListenForCommandsAsync(commandCancellation.Token);
    }

    private async Task ListenForCommandsAsync(CancellationToken cancellationToken)
    {
        WaitHandle[] handles = [showEvent!, settingsEvent!, updateEvent!, cancellationToken.WaitHandle];
        while (!cancellationToken.IsCancellationRequested)
        {
            int signaled = await Task.Run(() => WaitHandle.WaitAny(handles), cancellationToken);
            if (signaled == 3)
            {
                return;
            }

            await Dispatcher.InvokeAsync(() =>
            {
                if (signaled == 0)
                {
                    mainWindow?.ShowOverview();
                }
                else if (signaled == 1)
                {
                    mainWindow?.ShowSettings();
                }
                else if (signaled == 2 && mainWindow is not null)
                {
                    _ = mainWindow.CheckUpdatesAsync(true);
                }
            });
        }
    }

    private async Task CheckForUpdatesAfterStartupAsync()
    {
        await Task.Delay(TimeSpan.FromSeconds(8));
        try
        {
            GitHubRelease release = await UpdateService.FetchLatestAsync();
            if (UpdateService.IsNewerVersion(release.Version, UpdateService.CurrentVersion))
            {
                ShowTrayNotification("Mission Guard update available", $"{release.Version} is available. Open Mission Guard to install it.", false);
            }
        }
        catch (Exception error) when (error is HttpRequestException or IOException or InvalidOperationException or ArgumentException or System.Text.Json.JsonException)
        {
            AppLog.Write("Background update check failed: " + error.Message);
        }
    }

    private static void SignalExistingInstance(string[] arguments)
    {
        string eventName = arguments.Contains("--settings", StringComparer.OrdinalIgnoreCase)
            ? SettingsEventName
            : arguments.Contains("--check-updates", StringComparer.OrdinalIgnoreCase) ? UpdateEventName : ShowEventName;
        try
        {
            using EventWaitHandle existing = EventWaitHandle.OpenExisting(eventName);
            existing.Set();
        }
        catch (WaitHandleCannotBeOpenedException)
        {
        }
    }

    private static void ApplyTheme(IL2MissionGuard.Core.ThemeMode theme)
    {
        ApplicationTheme selected = theme switch
        {
            IL2MissionGuard.Core.ThemeMode.Light => ApplicationTheme.Light,
            IL2MissionGuard.Core.ThemeMode.Dark => ApplicationTheme.Dark,
            _ => ApplicationThemeManager.GetSystemTheme() == SystemTheme.Dark ? ApplicationTheme.Dark : ApplicationTheme.Light,
        };
        ApplicationThemeManager.Apply(selected, WindowBackdropType.None, true);
    }

    private static void OpenPath(string path)
    {
        try
        {
            if (Path.HasExtension(path))
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                if (!File.Exists(path))
                {
                    File.WriteAllText(path, string.Empty);
                }
            }
            else
            {
                Directory.CreateDirectory(path);
            }

            _ = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(path) { UseShellExecute = true });
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
        {
            AppLog.Write("Could not open path: " + error.Message);
        }
    }
}
