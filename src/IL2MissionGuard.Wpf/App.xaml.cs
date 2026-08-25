using System.Windows;
using IL2MissionGuard.Core;
using Wpf.Ui.Appearance;
using Wpf.Ui.Controls;

namespace IL2MissionGuard;

public partial class App : System.Windows.Application
{
    private const string MutexName = @"Local\IL2MissionGuard.UI.Client";
    private const string ShowEventName = @"Local\IL2MissionGuard.UI.Show";
    private const string SettingsEventName = @"Local\IL2MissionGuard.UI.Settings";
    private const string UpdateEventName = @"Local\IL2MissionGuard.UI.CheckUpdates";
    private Mutex? instanceMutex;
    private EventWaitHandle? showEvent;
    private EventWaitHandle? settingsEvent;
    private EventWaitHandle? updateEvent;
    private CancellationTokenSource? commandCancellation;
    private MissionGuardService? service;
    private MainWindow? mainWindow;

    internal bool IsShuttingDown { get; private set; }

    protected override void OnStartup(StartupEventArgs eventArgs)
    {
        base.OnStartup(eventArgs);
        HostInterop.Initialize(eventArgs.Args);

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
            service = new MissionGuardService(monitorOnly: true);
            mainWindow = new MainWindow(service);
            MainWindow = mainWindow;
            service.Start();
            StartCommandListener();

            if (eventArgs.Args.Contains("--settings", StringComparer.OrdinalIgnoreCase))
            {
                mainWindow.ShowSettings();
            }
            else if (eventArgs.Args.Contains("--check-updates", StringComparer.OrdinalIgnoreCase))
            {
                _ = mainWindow.CheckUpdatesAsync(true);
            }
            else
            {
                mainWindow.ShowOverview();
            }
        }
        catch (Exception error)
        {
            AppLog.Write("UI startup failed: " + error);
            System.Windows.MessageBox.Show(error.Message, "IL-2 Mission Guard", System.Windows.MessageBoxButton.OK, System.Windows.MessageBoxImage.Error);
            ExitApplication();
        }
    }

    internal void ExitApplication()
    {
        if (IsShuttingDown)
        {
            return;
        }

        IsShuttingDown = true;
        commandCancellation?.Cancel();
        service?.Dispose();
        service = null;
        Shutdown();
    }

    protected override void OnExit(ExitEventArgs eventArgs)
    {
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
            int signaled;
            try
            {
                signaled = await Task.Run(() => WaitHandle.WaitAny(handles), cancellationToken);
            }
            catch (OperationCanceledException)
            {
                return;
            }

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
}
