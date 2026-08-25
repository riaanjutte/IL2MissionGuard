using IL2MissionGuard.Core;

namespace IL2MissionGuard;

internal sealed record RecoveryRow(Snapshot Snapshot)
{
    public string CreatedLocal => Snapshot.CreatedUtc.LocalDateTime.ToString("dd MMM yyyy  HH:mm:ss", System.Globalization.CultureInfo.CurrentCulture);

    public string Mission => Path.GetFileNameWithoutExtension(Snapshot.MissionPath);

    public string Editor => Snapshot.EditorProcessName.Equals("STEditor", StringComparison.OrdinalIgnoreCase) ? "Great Battles" : "Korea";

    public int FileCount => Snapshot.Files.Count;

    public string Integrity => Snapshot.IsRestorable ? "Verified" : "Damaged";
}
