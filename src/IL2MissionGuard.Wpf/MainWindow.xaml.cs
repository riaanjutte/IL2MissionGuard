using System.ComponentModel;
using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using IL2MissionGuard.Core;
using Wpf.Ui.Appearance;
using Wpf.Ui.Controls;
using MessageBox = Wpf.Ui.Controls.MessageBox;

namespace IL2MissionGuard;

public partial class MainWindow : FluentWindow
{
    private readonly MissionGuardService service;
    private List<RecoveryRow> listedRecoveryRows = [];
    private int observedSnapshotCount = -1;
    private DateTimeOffset? observedNewestSnapshot;
    private GitHubRelease? availableRelease;

    internal MainWindow(MissionGuardService service)
    {
        this.service = service;
        InitializeComponent();
        VersionText.Text = "Version " + UpdateService.CurrentVersion;
        service.StatusChanged += Service_StatusChanged;
        LoadSettings();
        UpdateStatus();
    }

    internal void ShowOverview()
    {
        ShowPage(OverviewPage);
        Reveal();
    }

    internal void ShowSettings()
    {
        LoadSettings();
        ShowPage(SettingsPage);
        Reveal();
    }

    internal void ShowRecoveryPage()
    {
        RefreshRecoveryRows();
        ShowPage(RecoveryPage);
        Reveal();
    }

    internal async Task CheckUpdatesAsync(bool manual)
    {
        ShowPage(UpdatesPage);
        Reveal();
        UpdateButton.IsEnabled = false;
        UpdateTitleText.Text = "Checking for updates…";
        UpdateDetailText.Text = "Contacting the official GitHub release feed.";
        try
        {
            GitHubRelease release = await UpdateService.FetchLatestAsync();
            if (!UpdateService.IsNewerVersion(release.Version, UpdateService.CurrentVersion))
            {
                availableRelease = null;
                UpdateTitleText.Text = "IL-2 Mission Guard is up to date";
                UpdateDetailText.Text = $"You are running version {UpdateService.CurrentVersion}.";
                UpdateButton.Content = "Check again";
            }
            else
            {
                availableRelease = release;
                UpdateTitleText.Text = $"Version {release.Version.TrimStart('v', 'V')} is available";
                UpdateDetailText.Text = "The release can be downloaded, SHA-256 verified, installed, and restarted automatically.";
                UpdateButton.Content = "Install update";
            }
        }
        catch (Exception error) when (error is HttpRequestException or IOException or InvalidOperationException or ArgumentException or System.Text.Json.JsonException)
        {
            UpdateTitleText.Text = "Update check failed";
            UpdateDetailText.Text = error.Message;
            UpdateButton.Content = "Try again";
            AppLog.Write("Update check failed: " + error.Message);
        }
        finally
        {
            UpdateButton.IsEnabled = true;
        }
    }

    private void Reveal()
    {
        Show();
        if (WindowState == WindowState.Minimized)
        {
            WindowState = WindowState.Normal;
        }

        Activate();
        Topmost = true;
        Topmost = false;
        Focus();
    }

    private void Service_StatusChanged(object? sender, EventArgs eventArgs)
    {
        UpdateStatus();
        GuardStatus status = service.Status;
        if (RecoveryPage.Visibility == Visibility.Visible &&
            (status.SnapshotCount != observedSnapshotCount || status.LastSuccess != observedNewestSnapshot))
        {
            RefreshRecoveryRows();
        }
    }

    private void UpdateStatus()
    {
        GuardStatus status = service.Status;
        StateText.Text = status.StateText;
        StateDetailText.Text = status.LastIssue.Length > 0
            ? status.LastIssue
            : status.Editors.Count == 0 ? "Start either supported Mission Editor and Mission Guard will begin watching it." : "Mission Guard is monitoring the editor and saving on schedule.";
        EditorsText.Text = status.Editors.Count == 0
            ? "No editor detected"
            : string.Join("  •  ", status.Editors.Select(EditorDisplayName).Distinct());
        MissionsText.Text = status.Editors.Count == 0
            ? "Waiting in the notification area"
            : string.Join("  •  ", status.Editors.Select(editor => EditorInterop.TryGetSavedMissionPath(editor.Title, out string path, false) ? Path.GetFileName(path) : "Not saved yet"));
        LastSaveText.Text = status.LastSuccess is null
            ? "None this session"
            : status.LastSuccess.Value.LocalDateTime.ToString("dd MMM, HH:mm:ss", System.Globalization.CultureInfo.CurrentCulture);
        NextSaveText.Text = status.NextSave is null
            ? "No recovery point scheduled"
            : $"Next scheduled {RelativeTime(status.NextSave.Value)}";
        SnapshotCountText.Text = $"{status.SnapshotCount:N0} recovery point{(status.SnapshotCount == 1 ? string.Empty : "s")} stored";

        StatusIcon.Symbol = status.State switch
        {
            ProtectionState.Error => SymbolRegular.Warning24,
            ProtectionState.Protected => SymbolRegular.ShieldCheckmark24,
            ProtectionState.Disabled => SymbolRegular.ShieldDismiss24,
            _ => SymbolRegular.Shield24,
        };
        StatusIcon.Foreground = status.State == ProtectionState.Error
            ? new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(244, 96, 96))
            : new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(85, 168, 255));
    }

    private void LoadSettings()
    {
        AutoSaveOptions options = service.Options;
        GreatBattlesCheck.IsChecked = options.GreatBattles;
        KoreaCheck.IsChecked = options.Korea;
        IntervalBox.Value = options.IntervalMinutes;
        RetentionBox.Value = options.HistoricSnapshots;
        NotificationsCheck.IsChecked = options.TrayNotifications;
        ThemeCombo.SelectedIndex = options.Theme switch
        {
            IL2MissionGuard.Core.ThemeMode.Light => 1,
            IL2MissionGuard.Core.ThemeMode.Dark => 2,
            _ => 0,
        };
    }

    private void RefreshRecoveryRows()
    {
        listedRecoveryRows = service.GetSnapshots(ViewAllSnapshotsToggle.IsChecked == true)
            .Select(snapshot => new RecoveryRow(snapshot))
            .ToList();
        RecoveryGrid.ItemsSource = listedRecoveryRows;
        DeleteSnapshotsButton.IsEnabled = listedRecoveryRows.Count > 0;
        RestoreSnapshotButton.IsEnabled = listedRecoveryRows.Count > 0;
        GuardStatus status = service.Status;
        observedSnapshotCount = status.SnapshotCount;
        observedNewestSnapshot = status.LastSuccess;
    }

    private void ShowPage(FrameworkElement page)
    {
        OverviewPage.Visibility = Visibility.Collapsed;
        RecoveryPage.Visibility = Visibility.Collapsed;
        SettingsPage.Visibility = Visibility.Collapsed;
        UpdatesPage.Visibility = Visibility.Collapsed;
        page.Visibility = Visibility.Visible;
    }

    private void OverviewNav_Click(object sender, RoutedEventArgs eventArgs) => ShowPage(OverviewPage);

    private void RecoveryNav_Click(object sender, RoutedEventArgs eventArgs)
    {
        RefreshRecoveryRows();
        ShowPage(RecoveryPage);
    }

    private void SettingsNav_Click(object sender, RoutedEventArgs eventArgs)
    {
        LoadSettings();
        ShowPage(SettingsPage);
    }

    private void UpdatesNav_Click(object sender, RoutedEventArgs eventArgs) => ShowPage(UpdatesPage);

    private void CloseWindow_Click(object sender, RoutedEventArgs eventArgs) => Close();

    private async void CreateRecovery_Click(object sender, RoutedEventArgs eventArgs)
    {
        if (sender is System.Windows.Controls.Button button)
        {
            button.IsEnabled = false;
            try
            {
                IReadOnlyList<SaveAttemptResult> results = await service.CreateRecoveryPointNowAsync();
                string summary = string.Join(Environment.NewLine + Environment.NewLine, results.Select(result =>
                    $"{(result.Success ? "Created" : result.Failure ? "Failed" : "Skipped")} — {result.Subject}{Environment.NewLine}{result.Detail}"));
                await ShowMessageAsync("Create recovery point", summary);
                UpdateStatus();
            }
            finally
            {
                button.IsEnabled = true;
            }
        }
    }

    private void RefreshRecovery_Click(object sender, RoutedEventArgs eventArgs) => RefreshRecoveryRows();

    private void ViewAllSnapshots_Changed(object sender, RoutedEventArgs eventArgs)
    {
        if (IsInitialized)
        {
            RefreshRecoveryRows();
        }
    }

    private void OpenSnapshots_Click(object sender, RoutedEventArgs eventArgs) => OpenPath(SettingsStore.DefaultSnapshotRoot, true);

    private async void DeleteSnapshots_Click(object sender, RoutedEventArgs eventArgs)
    {
        List<Snapshot> snapshots = listedRecoveryRows.Select(row => row.Snapshot).ToList();
        int count = snapshots.Count;
        if (count == 0)
        {
            RefreshRecoveryRows();
            return;
        }

        Wpf.Ui.Controls.MessageBoxResult answer = await ShowMessageAsync(
            "Delete listed snapshots?",
            $"Permanently delete the {count:N0} recovery point{(count == 1 ? string.Empty : "s")} currently listed?{Environment.NewLine}{Environment.NewLine}" +
            "Snapshots hidden by the current filter and restore safety copies will not be deleted." + Environment.NewLine + Environment.NewLine +
            "This action cannot be undone.",
            "Delete listed snapshots",
            "Cancel",
            ControlAppearance.Danger);
        if (answer != Wpf.Ui.Controls.MessageBoxResult.Primary)
        {
            return;
        }

        try
        {
            IsEnabled = false;
            _ = await service.DeleteSnapshotsAsync(snapshots);
            RefreshRecoveryRows();
            UpdateStatus();
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidOperationException)
        {
            AppLog.Write("Snapshot deletion failed: " + error.Message);
            await ShowMessageAsync("Snapshots could not be deleted", error.Message);
        }
        finally
        {
            IsEnabled = true;
        }
    }

    private async void Restore_Click(object sender, RoutedEventArgs eventArgs)
    {
        if (RecoveryGrid.SelectedItem is not RecoveryRow row || !row.Snapshot.IsRestorable)
        {
            await ShowMessageAsync("Restore recovery point", "Select a verified recovery point first.");
            return;
        }

        Wpf.Ui.Controls.MessageBoxResult answer = await ShowMessageAsync(
            "Restore IL-2 mission recovery point",
            $"Restore {Path.GetFileName(row.Snapshot.MissionPath)} to the recovery point from {row.Snapshot.CreatedUtc.LocalDateTime:dd MMM yyyy, HH:mm:ss}?{Environment.NewLine}{Environment.NewLine}" +
            "If the mission is open, its editor will close first. If asked whether to save current changes, choose Don’t Save. A separate safety copy is created before any file is replaced.",
            "Restore",
            "Cancel");
        if (answer != Wpf.Ui.Controls.MessageBoxResult.Primary)
        {
            return;
        }

        try
        {
            IsEnabled = false;
            (RestoreResult result, bool reopened, string diagnostic) = await service.RestoreAsync(row.Snapshot);
            RefreshRecoveryRows();
            if (!reopened)
            {
                await ShowMessageAsync(
                    "Mission reopen failed",
                    $"The mission was restored, but it could not be reopened automatically.{Environment.NewLine}{Environment.NewLine}{diagnostic}{Environment.NewLine}{Environment.NewLine}Safety copy:{Environment.NewLine}{result.SafetyBackupDirectory}");
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidOperationException or TimeoutException or OperationCanceledException)
        {
            AppLog.Write("Mission restore failed: " + error.Message);
            await ShowMessageAsync("Mission recovery failed", "The mission was not restored." + Environment.NewLine + Environment.NewLine + error.Message);
        }
        finally
        {
            IsEnabled = true;
        }
    }

    private async void SaveSettings_Click(object sender, RoutedEventArgs eventArgs)
    {
        bool greatBattles = GreatBattlesCheck.IsChecked == true;
        bool korea = KoreaCheck.IsChecked == true;
        int interval = (int)Math.Round(IntervalBox.Value ?? 5);
        int retention = (int)Math.Round(RetentionBox.Value ?? 10);
        IL2MissionGuard.Core.ThemeMode theme = ThemeCombo.SelectedItem is ComboBoxItem item && Enum.TryParse(item.Tag?.ToString(), out IL2MissionGuard.Core.ThemeMode selected)
            ? selected
            : IL2MissionGuard.Core.ThemeMode.System;
        AutoSaveOptions options = new(
            Enabled: greatBattles || korea,
            GreatBattles: greatBattles,
            Korea: korea,
            IntervalMinutes: interval,
            HistoricSnapshots: retention,
            TrayNotifications: NotificationsCheck.IsChecked == true,
            Theme: theme);
        try
        {
            SettingsStore.Save(SettingsStore.DefaultSettingsPath, options);
            ApplyTheme(theme);
            service.ReloadSettings();
            await ShowMessageAsync("Settings saved", "Your Mission Guard settings have been updated.");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidOperationException or ArgumentOutOfRangeException)
        {
            await ShowMessageAsync("Settings could not be saved", error.Message);
        }
    }

    private void OpenSettingsFile_Click(object sender, RoutedEventArgs eventArgs) => OpenPath(SettingsStore.DefaultSettingsPath, false);

    private async void CheckUpdates_Click(object sender, RoutedEventArgs eventArgs)
    {
        if (availableRelease is null)
        {
            await CheckUpdatesAsync(true);
            return;
        }

        Wpf.Ui.Controls.MessageBoxResult answer = await ShowMessageAsync(
            "Install Mission Guard update",
            $"Download, verify, install, and restart Mission Guard {availableRelease.Version}?",
            "Install",
            "Cancel");
        if (answer != Wpf.Ui.Controls.MessageBoxResult.Primary)
        {
            return;
        }

        UpdateButton.IsEnabled = false;
        UpdateTitleText.Text = "Continuing in the notification area…";
        try
        {
            HostInterop.RequestUpdateCheck();
            ((App)System.Windows.Application.Current).ExitApplication();
        }
        catch (InvalidOperationException error)
        {
            UpdateTitleText.Text = "Update installation failed";
            UpdateDetailText.Text = error.Message;
            UpdateButton.IsEnabled = true;
            await ShowMessageAsync("Mission Guard update", "The tray host could not continue the update." + Environment.NewLine + Environment.NewLine + error.Message);
        }
    }

    private static string EditorDisplayName(EditorProcess editor) =>
        editor.Name.Equals("STEditor", StringComparison.OrdinalIgnoreCase) ? "Great Battles" : "Korea";

    private static string RelativeTime(DateTimeOffset target)
    {
        TimeSpan remaining = target - DateTimeOffset.Now;
        if (remaining <= TimeSpan.Zero)
        {
            return "now";
        }

        return remaining.TotalMinutes >= 1 ? $"in {Math.Ceiling(remaining.TotalMinutes):N0} min" : $"in {Math.Max(1, Math.Ceiling(remaining.TotalSeconds)):N0} sec";
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

    private static void OpenPath(string path, bool directory)
    {
        try
        {
            if (directory)
            {
                Directory.CreateDirectory(path);
            }
            else if (!File.Exists(path))
            {
                SettingsStore.Save(path, new AutoSaveOptions());
            }

            _ = Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or Win32Exception)
        {
            AppLog.Write("Could not open path: " + error.Message);
        }
    }

    private Task<Wpf.Ui.Controls.MessageBoxResult> ShowMessageAsync(
        string title,
        string message,
        string primary = "OK",
        string? secondary = null,
        ControlAppearance primaryAppearance = ControlAppearance.Primary)
    {
        MessageBox box = new()
        {
            Title = title,
            Content = message,
            PrimaryButtonText = secondary is null ? string.Empty : primary,
            CloseButtonText = secondary ?? primary,
            PrimaryButtonAppearance = primaryAppearance,
            Owner = this,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
        };
        return box.ShowDialogAsync();
    }
}
