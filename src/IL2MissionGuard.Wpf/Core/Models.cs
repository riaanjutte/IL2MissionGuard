using System.Text.Json.Serialization;

namespace IL2MissionGuard.Core;

internal enum ThemeMode
{
    System,
    Light,
    Dark,
}

internal sealed record AutoSaveOptions(
    bool Enabled = true,
    bool GreatBattles = true,
    bool Korea = true,
    int IntervalMinutes = 5,
    int HistoricSnapshots = 10,
    bool TrayNotifications = true,
    ThemeMode Theme = ThemeMode.System);

internal sealed class SnapshotFile
{
    public string OriginalFileName { get; set; } = string.Empty;

    public string SnapshotFileName { get; set; } = string.Empty;

    public long Length { get; set; }

    public string Sha256 { get; set; } = string.Empty;
}

internal sealed class Snapshot
{
    public int SchemaVersion { get; set; } = 1;

    public string MissionPath { get; set; } = string.Empty;

    public string EditorProcessName { get; set; } = string.Empty;

    public string EditorExecutablePath { get; set; } = string.Empty;

    public DateTimeOffset CreatedUtc { get; set; }

    public List<SnapshotFile> Files { get; set; } = [];

    [JsonIgnore]
    public string MetadataPath { get; set; } = string.Empty;

    [JsonIgnore]
    public string IntegrityError { get; set; } = string.Empty;

    [JsonIgnore]
    public bool IsRestorable => string.IsNullOrEmpty(IntegrityError);
}

internal sealed record RestoreResult(string MissionPath, string SafetyBackupDirectory, int RestoredFileCount);

internal sealed record EditorProcess(int Id, string Name, string ExecutablePath, nint WindowHandle, string Title);

internal sealed record SaveAttemptResult(bool Success, bool Failure, string Subject, string Detail);

internal sealed record GitHubRelease(string Version, string ReleaseUrl, string AssetUrl, string Sha256);

internal enum ProtectionState
{
    Disabled,
    Idle,
    Waiting,
    Protected,
    Error,
}

internal sealed record GuardStatus(
    ProtectionState State,
    string StateText,
    IReadOnlyList<EditorProcess> Editors,
    DateTimeOffset? LastSuccess,
    string LastSuccessMission,
    DateTimeOffset? NextSave,
    int SnapshotCount,
    string LastIssue);

internal sealed record GuardNotification(string Title, string Message, bool IsError = false);
