using IL2MissionGuard.Core;

string root = Path.Combine(Path.GetTempPath(), $"IL2MissionGuard-ManagedTests-{Environment.ProcessId}-{Environment.TickCount64}");
Directory.CreateDirectory(root);

try
{
    TestSettings(root);
    TestMissionTitle(root);
    TestUpdateMetadata();
    await TestSnapshotsAsync(root);
    TestLegacyImport(root);
    Directory.Delete(root, true);
    Console.WriteLine("All managed Mission Guard regression tests passed.");
    return 0;
}
catch (Exception error)
{
    Console.Error.WriteLine("FAILED: " + error);
    Console.Error.WriteLine("Test data retained at " + root);
    return 1;
}

static void TestSettings(string root)
{
    string ini = Path.Combine(root, "settings.ini");
    File.WriteAllText(ini, "[AutoSave]\nEnabled=true\nGreatBattles=false\nKorea=true\nIntervalMinutes=7\nHistoricSnapshots=12\nTrayNotifications=false\nTheme=Dark\n");
    AutoSaveOptions options = SettingsStore.Load(ini);
    Require(options is { Enabled: true, GreatBattles: false, Korea: true, IntervalMinutes: 7, HistoricSnapshots: 12, TrayNotifications: false, Theme: ThemeMode.Dark }, "settings parse");

    File.WriteAllText(ini, "[EditorDimensions]\nMissionTreeWidth=333\n\n[KoreaOptions]\nForceViewportToSdr=true\n\n[AutoSave]\nEnabled=true\nIntervalMinutes=5\n");
    AutoSaveOptions requested = new(true, false, true, 17, 42, false, ThemeMode.Light);
    SettingsStore.Save(ini, requested);
    Require(SettingsStore.Load(ini) == requested, "settings round trip");
    string saved = File.ReadAllText(ini);
    Require(saved.Contains("MissionTreeWidth=333", StringComparison.Ordinal) && saved.Contains("ForceViewportToSdr=true", StringComparison.Ordinal), "unrelated INI sections preserved");

    File.WriteAllText(ini, "[AutoSave]\nIntervalMinutes=999\n");
    Require(SettingsStore.Load(ini) == new AutoSaveOptions(), "invalid settings fallback");
}

static void TestMissionTitle(string root)
{
    string mission = Path.GetFullPath(Path.Combine(root, "Title Mission.Mission"));
    File.WriteAllText(mission, "mission");
    Require(EditorInterop.TryGetSavedMissionPath("IL2 Series Editor - " + mission + " *", out string parsed) && parsed == mission, "dirty mission title parse");
    Require(EditorInterop.TryGetSavedMissionPath("Editor - " + Path.ChangeExtension(mission, ".msnbin"), out parsed) && parsed == mission, "msnbin canonicalization");
    Require(!EditorInterop.TryGetSavedMissionPath("Editor - <empty>", out _), "empty title rejection");
}

static void TestUpdateMetadata()
{
    Require(UpdateService.IsNewerVersion("v2.0.0", "1.99.99"), "version comparison");
    string json = """
        {
          "tag_name":"v2.1.0",
          "html_url":"https://github.com/riaanjutte/IL2MissionGuard/releases/tag/v2.1.0",
          "assets":[{
            "name":"IL2MissionGuard.exe",
            "browser_download_url":"https://github.com/riaanjutte/IL2MissionGuard/releases/download/v2.1.0/IL2MissionGuard.exe",
            "digest":"sha256:d62319688f4f86f4d70a555e794eb5f673fa6ef1fadb6a48780b0d413b171b19"
          }]
        }
        """;
    GitHubRelease release = UpdateService.ParseLatestRelease(json);
    Require(release.Version == "v2.1.0" && release.Sha256.Length == 64, "release metadata parse");
    bool rejected = false;
    try
    {
        _ = UpdateService.ParseLatestRelease(json.Replace("https://github.com/riaanjutte/IL2MissionGuard/releases/download/", "https://example.com/", StringComparison.Ordinal));
    }
    catch (InvalidOperationException)
    {
        rejected = true;
    }

    Require(rejected, "untrusted release URL rejected");
}

static async Task TestSnapshotsAsync(string root)
{
    string missions = Path.Combine(root, "Missions");
    Directory.CreateDirectory(missions);
    string mission = Path.Combine(missions, "RecoveryTest.Mission");
    string binary = Path.Combine(missions, "RecoveryTest.msnbin");
    string localization = Path.Combine(missions, "RecoveryTest.eng");
    File.WriteAllText(mission, "version-one");
    File.WriteAllText(binary, "compiled-one");
    File.WriteAllText(localization, "english-one");
    await SnapshotStore.WaitUntilMissionFamilyStableAsync(mission, CancellationToken.None, TimeSpan.FromSeconds(1), TimeSpan.FromMilliseconds(10), 3, TimeSpan.FromMilliseconds(20));

    SnapshotStore store = new(Path.Combine(root, "Autosave"), 2);
    string fakeEditor = Path.Combine(root, "IL2Editor.exe");
    File.WriteAllText(fakeEditor, "fake");
    _ = store.CreateSnapshot(mission, "IL2Editor", fakeEditor, DateTimeOffset.UtcNow.AddSeconds(-3));
    File.WriteAllText(mission, "version-two");
    File.WriteAllText(binary, "compiled-two");
    Snapshot second = store.CreateSnapshot(mission, "IL2Editor", fakeEditor, DateTimeOffset.UtcNow.AddSeconds(-2));
    File.WriteAllText(mission, "version-three");
    _ = store.CreateSnapshot(mission, "IL2Editor", fakeEditor, DateTimeOffset.UtcNow.AddSeconds(-1));
    List<Snapshot> retained = store.ListSnapshots(mission, 10);
    Require(retained.Count == 2 && store.CountSnapshots() == 2, "per-mission retention");

    File.WriteAllText(mission, "unwanted-current");
    File.WriteAllText(binary, "unwanted-compiled");
    RestoreResult restored = store.RestoreSnapshot(second);
    Require(File.ReadAllText(mission) == "version-two" && File.ReadAllText(binary) == "compiled-two", "mission family restore");
    Require(Directory.Exists(restored.SafetyBackupDirectory) && File.ReadAllText(Path.Combine(restored.SafetyBackupDirectory, Path.GetFileName(mission))) == "unwanted-current", "pre-restore safety backup");

    Snapshot damageTarget = store.ListSnapshots(mission, 10)[0];
    File.WriteAllText(Path.Combine(Path.GetDirectoryName(damageTarget.MetadataPath)!, damageTarget.Files[0].SnapshotFileName), "tampered");
    Snapshot damaged = store.ListSnapshots(mission, 10)[0];
    Require(!damaged.IsRestorable, "checksum damage detected");
    bool rejected = false;
    try
    {
        _ = store.RestoreSnapshot(damaged);
    }
    catch (InvalidOperationException)
    {
        rejected = true;
    }

    Require(rejected, "damaged restore rejected");

    int deleted = store.DeleteAllSnapshots();
    Require(deleted == 2, "delete all reports snapshot count");
    Require(store.CountSnapshots() == 0 && Directory.Exists(store.RootDirectory), "delete all clears and recreates snapshot root");
}

static void TestLegacyImport(string root)
{
    string legacy = Path.Combine(root, "Legacy");
    string destination = Path.Combine(root, "Imported");
    Directory.CreateDirectory(Path.Combine(legacy, "Mission_ABC"));
    File.WriteAllText(Path.Combine(legacy, "Mission_ABC", "old.Mission"), "old");
    File.WriteAllText(Path.Combine(legacy, "Mission_ABC", "ignored.tmp-123"), "temporary");
    SnapshotStore store = new(destination, 10);
    Require(store.ImportLegacySnapshots(legacy) == 1, "legacy import");
    Require(store.ImportLegacySnapshots(legacy) == 0, "legacy import idempotence");
}

static void Require(bool condition, string message)
{
    if (!condition)
    {
        throw new InvalidOperationException(message);
    }
}
