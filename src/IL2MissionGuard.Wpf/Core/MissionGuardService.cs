using System.Collections.Concurrent;
using System.Windows.Threading;

namespace IL2MissionGuard.Core;

internal sealed class MissionGuardService : IDisposable
{
    private readonly DispatcherTimer timer;
    private readonly SemaphoreSlim operationLock = new(1, 1);
    private readonly ConcurrentDictionary<int, DateTimeOffset> nextSave = new();
    private readonly HashSet<int> protectedProcesses = [];
    private readonly Dictionary<int, string> processIssues = [];
    private readonly EventWaitHandle stopEvent;
    private AutoSaveOptions options;
    private SnapshotStore store;
    private List<EditorProcess> editors = [];
    private DateTimeOffset? lastSuccess;
    private string lastSuccessMission = string.Empty;
    private int snapshotCount;
    private bool disposed;

    public MissionGuardService()
    {
        Directory.CreateDirectory(SettingsStore.LocalAppDataDirectory);
        options = SettingsStore.Load(SettingsStore.DefaultSettingsPath);
        store = new SnapshotStore(SettingsStore.DefaultSnapshotRoot, options.HistoricSnapshots);
        stopEvent = new EventWaitHandle(false, EventResetMode.AutoReset, @"Local\IL2MEC.AutoSave.Stop");
        try
        {
            int imported = store.ImportLegacySnapshots(SettingsStore.LegacySnapshotRoot);
            if (imported > 0)
            {
                AppLog.Write($"Imported {imported} legacy autosave files. The legacy copies were retained.");
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidOperationException)
        {
            AppLog.Write("Could not import legacy autosave files: " + error.Message);
        }

        snapshotCount = store.CountSnapshots();

        timer = new DispatcherTimer(DispatcherPriority.Background)
        {
            Interval = TimeSpan.FromSeconds(2),
        };
        timer.Tick += OnTimerTick;
    }

    public event EventHandler? StatusChanged;

    public event EventHandler<GuardNotification>? NotificationRaised;

    public AutoSaveOptions Options => options;

    public GuardStatus Status
    {
        get
        {
            ProtectionState state = CurrentProtectionState();
            string text = state switch
            {
                ProtectionState.Disabled => "Autosave is disabled",
                ProtectionState.Idle => "Ready — waiting for a supported Mission Editor",
                ProtectionState.Waiting => "Waiting for the first recovery point this session",
                ProtectionState.Protected => "Protected — automatic recovery points are active",
                _ => "Attention needed — the last recovery attempt failed",
            };
            DateTimeOffset? next = nextSave.IsEmpty ? null : nextSave.Values.Min();
            return new GuardStatus(state, text, editors, lastSuccess, lastSuccessMission, next, snapshotCount, LastIssue());
        }
    }

    public void Start()
    {
        Tick();
        timer.Start();
    }

    public void ReloadSettings()
    {
        AutoSaveOptions loaded = SettingsStore.Load(SettingsStore.DefaultSettingsPath);
        bool scheduleChanged = loaded.Enabled != options.Enabled || loaded.IntervalMinutes != options.IntervalMinutes ||
            loaded.GreatBattles != options.GreatBattles || loaded.Korea != options.Korea;
        bool retentionChanged = loaded.HistoricSnapshots != options.HistoricSnapshots;
        options = loaded;
        if (scheduleChanged)
        {
            nextSave.Clear();
        }

        if (retentionChanged)
        {
            store = new SnapshotStore(SettingsStore.DefaultSnapshotRoot, options.HistoricSnapshots);
            store.PruneToRetentionLimit();
            snapshotCount = store.CountSnapshots();
        }

        Tick();
    }

    public List<Snapshot> GetSnapshots(int maximum = 200)
    {
        List<string> active = editors
            .Select(editor => EditorInterop.TryGetSavedMissionPath(editor.Title, out string path) ? path : null)
            .OfType<string>()
            .ToList();
        if (active.Count == 0)
        {
            return store.ListSnapshots(null, maximum);
        }

        return active.SelectMany(path => store.ListSnapshots(path, options.HistoricSnapshots))
            .OrderByDescending(snapshot => snapshot.CreatedUtc)
            .Take(maximum)
            .ToList();
    }

    public async Task<IReadOnlyList<SaveAttemptResult>> CreateRecoveryPointNowAsync(CancellationToken cancellationToken = default)
    {
        editors = EditorInterop.FindEditors(options);
        if (editors.Count == 0)
        {
            return [new SaveAttemptResult(false, false, "Mission Editors", "No enabled IL-2 Mission Editor is currently running.")];
        }

        if (!await operationLock.WaitAsync(0, cancellationToken))
        {
            return [new SaveAttemptResult(false, false, "Mission Guard", "A recovery operation is already in progress.")];
        }

        try
        {
            List<SaveAttemptResult> results = [];
            foreach (EditorProcess editor in editors)
            {
                results.Add(await TrySaveAsync(editor, true, cancellationToken));
                nextSave[editor.Id] = DateTimeOffset.Now.AddMinutes(options.IntervalMinutes);
            }

            return results;
        }
        finally
        {
            operationLock.Release();
            RaiseStatusChanged();
        }
    }

    public async Task<(RestoreResult Result, bool Reopened, string Diagnostic)> RestoreAsync(Snapshot snapshot, CancellationToken cancellationToken = default)
    {
        await operationLock.WaitAsync(cancellationToken);
        try
        {
            EditorProcess? editor = EditorInterop.FindEditorForSnapshot(snapshot, options);
            if (editor is not null)
            {
                await EditorInterop.CloseEditorAsync(editor, cancellationToken);
            }

            RestoreResult result = store.RestoreSnapshot(snapshot);
            (bool reopened, string diagnostic) = await EditorInterop.OpenMissionAsync(
                snapshot.EditorExecutablePath,
                snapshot.EditorProcessName,
                result.MissionPath,
                cancellationToken);
            AppLog.Write(reopened
                ? $"Restored {Path.GetFileName(result.MissionPath)} and reopened it. Safety backup: {result.SafetyBackupDirectory}"
                : $"Restored {Path.GetFileName(result.MissionPath)}, but reopen failed: {diagnostic}. Safety backup: {result.SafetyBackupDirectory}");
            return (result, reopened, diagnostic);
        }
        finally
        {
            operationLock.Release();
            RaiseStatusChanged();
        }
    }

    public void Dispose()
    {
        if (disposed)
        {
            return;
        }

        disposed = true;
        timer.Stop();
        stopEvent.Dispose();
        operationLock.Dispose();
    }

    private void OnTimerTick(object? sender, EventArgs eventArgs) => Tick();

    private void Tick()
    {
        if (stopEvent.WaitOne(0))
        {
            System.Windows.Application.Current.Shutdown();
            return;
        }

        AutoSaveOptions loaded = SettingsStore.Load(SettingsStore.DefaultSettingsPath);
        if (loaded != options)
        {
            ReloadSettings();
            return;
        }

        editors = EditorInterop.FindEditors(options);
        HashSet<int> live = editors.Select(editor => editor.Id).ToHashSet();
        nextSave.Keys.Where(id => !live.Contains(id)).ToList().ForEach(id => nextSave.TryRemove(id, out _));
        protectedProcesses.RemoveWhere(id => !live.Contains(id));
        foreach (int id in processIssues.Keys.Where(id => !live.Contains(id)).ToList())
        {
            processIssues.Remove(id);
        }

        DateTimeOffset now = DateTimeOffset.Now;
        foreach (EditorProcess editor in editors)
        {
            if (!nextSave.TryGetValue(editor.Id, out DateTimeOffset due))
            {
                nextSave[editor.Id] = now.AddMinutes(options.IntervalMinutes);
            }
            else if (options.Enabled && due <= now)
            {
                nextSave[editor.Id] = now.AddMinutes(options.IntervalMinutes);
                _ = SaveInBackgroundAsync(editor);
            }
        }

        RaiseStatusChanged();
    }

    private async Task SaveInBackgroundAsync(EditorProcess editor)
    {
        if (!await operationLock.WaitAsync(0))
        {
            return;
        }

        try
        {
            _ = await TrySaveAsync(editor, false, CancellationToken.None);
        }
        finally
        {
            operationLock.Release();
            RaiseStatusChanged();
        }
    }

    private async Task<SaveAttemptResult> TrySaveAsync(EditorProcess editor, bool manual, CancellationToken cancellationToken)
    {
        string editorName = editor.Name.Equals("STEditor", StringComparison.OrdinalIgnoreCase) ? "Great Battles" : "Korea";
        if (!NativeMethods.IsWindowEnabled(editor.WindowHandle))
        {
            return new SaveAttemptResult(false, false, editorName, "Skipped because the editor has a dialog open.");
        }

        if (!EditorInterop.TryGetSavedMissionPath(editor.Title, out string mission))
        {
            AppLog.Write($"Skipped {editor.Name}: the mission has not been saved to a named file yet.");
            return new SaveAttemptResult(false, false, editorName, "The mission needs one manual save before recovery points can be created.");
        }

        string subject = Path.GetFileName(mission);
        if (!EditorInterop.SendSave(editor))
        {
            return Failure(editor, subject, "The Mission Editor did not complete its Save command.", manual);
        }

        if (string.IsNullOrEmpty(editor.ExecutablePath))
        {
            return Failure(editor, subject, "The editor path was unavailable, so a restorable recovery point could not be created.", manual);
        }

        try
        {
            await SnapshotStore.WaitUntilMissionFamilyStableAsync(mission, cancellationToken);
            Exception? lastError = null;
            for (int attempt = 1; attempt <= 4; attempt++)
            {
                try
                {
                    store.CreateSnapshot(mission, editor.Name, editor.ExecutablePath);
                    lastError = null;
                    break;
                }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidOperationException)
                {
                    lastError = error;
                    if (attempt < 4)
                    {
                        await Task.Delay(250, cancellationToken);
                    }
                }
            }

            if (lastError is not null)
            {
                throw lastError;
            }

            lastSuccess = DateTimeOffset.Now;
            lastSuccessMission = subject;
            snapshotCount = store.CountSnapshots();
            processIssues.Remove(editor.Id);
            protectedProcesses.Add(editor.Id);
            AppLog.Write($"Recovery point created for {subject}.");
            if (!manual && options.TrayNotifications)
            {
                RaiseNotification(new GuardNotification("IL-2 Mission Guard", $"Recovery point created for {subject}."));
            }

            return new SaveAttemptResult(true, false, subject, "Recovery point created.");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidOperationException or TimeoutException)
        {
            AppLog.Write($"Autosave saved {mission}, but its recovery point failed: {error.Message}");
            return Failure(editor, subject, "The mission was saved, but its timestamped recovery copy failed. See the autosave log.", manual);
        }
    }

    private SaveAttemptResult Failure(EditorProcess editor, string subject, string detail, bool manual)
    {
        processIssues[editor.Id] = subject + ": " + detail;
        AppLog.Write($"Recovery point failed for {subject}: {detail}");
        if (!manual)
        {
            RaiseNotification(new GuardNotification("Mission Guard recovery point failed", processIssues[editor.Id], true));
        }

        return new SaveAttemptResult(false, true, subject, detail);
    }

    private ProtectionState CurrentProtectionState()
    {
        if (!options.Enabled || (!options.GreatBattles && !options.Korea))
        {
            return ProtectionState.Disabled;
        }

        if (processIssues.Count > 0)
        {
            return ProtectionState.Error;
        }

        if (editors.Count == 0)
        {
            return ProtectionState.Idle;
        }

        return editors.All(editor => protectedProcesses.Contains(editor.Id)) ? ProtectionState.Protected : ProtectionState.Waiting;
    }

    private string LastIssue() => processIssues.Values.FirstOrDefault() ?? string.Empty;

    private void RaiseStatusChanged() => System.Windows.Application.Current?.Dispatcher.BeginInvoke(() => StatusChanged?.Invoke(this, EventArgs.Empty));

    private void RaiseNotification(GuardNotification notification) =>
        System.Windows.Application.Current?.Dispatcher.BeginInvoke(() => NotificationRaised?.Invoke(this, notification));
}
