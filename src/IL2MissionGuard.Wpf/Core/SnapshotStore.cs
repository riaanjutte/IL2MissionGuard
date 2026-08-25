using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace IL2MissionGuard.Core;

internal sealed class SnapshotStore
{
    private const string MetadataSuffix = ".il2mec-autosave.json";
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = false,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
    };

    public SnapshotStore(string rootDirectory, int retentionCount)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(retentionCount, 1);

        RootDirectory = Path.GetFullPath(rootDirectory);
        RetentionCount = retentionCount;
    }

    public string RootDirectory { get; }

    public int RetentionCount { get; }

    public int ImportLegacySnapshots(string legacyRootDirectory)
    {
        string legacy = Path.GetFullPath(legacyRootDirectory);
        if (!Directory.Exists(legacy) || PathEquals(legacy, RootDirectory))
        {
            return 0;
        }

        int imported = 0;
        foreach (string source in Directory.EnumerateFiles(legacy, "*", SearchOption.AllDirectories))
        {
            FileInfo info = new(source);
            if (info.LinkTarget is not null || info.Name.Contains(".tmp-", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            string relative = Path.GetRelativePath(legacy, source);
            if (relative.StartsWith("..", StringComparison.Ordinal))
            {
                continue;
            }

            string destination = Path.Combine(RootDirectory, relative);
            if (File.Exists(destination))
            {
                continue;
            }

            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            CopyFileAtomically(source, destination, false);
            imported++;
        }

        return imported;
    }

    public Snapshot CreateSnapshot(
        string missionValue,
        string editorProcessName,
        string editorExecutablePath,
        DateTimeOffset? createdUtc = null)
    {
        string missionPath = NormalizeMissionPath(missionValue, true);
        if (string.IsNullOrWhiteSpace(editorProcessName) || string.IsNullOrWhiteSpace(editorExecutablePath))
        {
            throw new ArgumentException("Editor identity was incomplete.");
        }

        List<string> sourceFiles = EnumerateMissionFamily(missionPath);
        if (!sourceFiles.Any(path => PathEquals(path, missionPath)))
        {
            throw new InvalidOperationException("The mission file was not found in its companion file set.");
        }

        DateTimeOffset timestamp = createdUtc ?? DateTimeOffset.UtcNow;
        string directory = Path.Combine(RootDirectory, BuildMissionKey(missionPath));
        Directory.CreateDirectory(directory);
        string proposed = $"{SanitizeFileName(Path.GetFileNameWithoutExtension(missionPath))}_{timestamp.LocalDateTime:yyyy-MM-dd_HH-mm-ss}-{timestamp.Millisecond:000}";
        string prefix = proposed;
        for (int suffix = 2; File.Exists(Path.Combine(directory, prefix + MetadataSuffix)); suffix++)
        {
            prefix = $"{proposed}_{suffix}";
        }

        Snapshot snapshot = new()
        {
            MissionPath = missionPath,
            EditorProcessName = editorProcessName,
            EditorExecutablePath = Path.GetFullPath(editorExecutablePath),
            CreatedUtc = timestamp.ToUniversalTime(),
            MetadataPath = Path.Combine(directory, prefix + MetadataSuffix),
        };

        List<string> copied = [];
        try
        {
            foreach (string source in sourceFiles)
            {
                string storedName = prefix + Path.GetExtension(source);
                string destination = Path.Combine(directory, storedName);
                CopyFileAtomically(source, destination, false);
                copied.Add(destination);
                snapshot.Files.Add(new SnapshotFile
                {
                    OriginalFileName = Path.GetFileName(source),
                    SnapshotFileName = storedName,
                    Length = new FileInfo(destination).Length,
                    Sha256 = Sha256File(destination),
                });
            }

            WriteUtf8Atomically(snapshot.MetadataPath, JsonSerializer.Serialize(snapshot, JsonOptions) + Environment.NewLine);
            EnforceRetention(directory);
            return snapshot;
        }
        catch
        {
            foreach (string file in copied)
            {
                TryDelete(file);
            }

            TryDelete(snapshot.MetadataPath);
            throw;
        }
    }

    public List<Snapshot> ListSnapshots(string? missionPath = null, int maximum = int.MaxValue)
    {
        List<Snapshot> result = [];
        if (!Directory.Exists(RootDirectory) || maximum < 1)
        {
            return result;
        }

        string? normalized = missionPath is null ? null : NormalizeMissionPath(missionPath, false);
        foreach (string metadata in Directory.EnumerateFiles(RootDirectory, "*" + MetadataSuffix, SearchOption.AllDirectories))
        {
            try
            {
                Snapshot snapshot = ReadMetadata(metadata);
                if (normalized is null || PathEquals(snapshot.MissionPath, normalized))
                {
                    result.Add(AssessIntegrity(snapshot));
                }
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }
            catch (JsonException)
            {
            }
            catch (InvalidOperationException)
            {
            }
        }

        return result.OrderByDescending(item => item.CreatedUtc).Take(maximum).ToList();
    }

    public int CountSnapshots() => Directory.Exists(RootDirectory)
        ? Directory.EnumerateFiles(RootDirectory, "*" + MetadataSuffix, SearchOption.AllDirectories).Count()
        : 0;

    public int DeleteSnapshots(IEnumerable<Snapshot> snapshots)
    {
        int deleted = 0;
        HashSet<string> metadataPaths = new(StringComparer.OrdinalIgnoreCase);
        foreach (Snapshot snapshot in snapshots)
        {
            ValidateSnapshot(snapshot);
            string metadata = Path.GetFullPath(snapshot.MetadataPath);
            EnsureInside(RootDirectory, metadata, "Snapshot is outside the recovery folder.");
            if (!metadataPaths.Add(metadata) || !File.Exists(metadata))
            {
                continue;
            }

            string directory = Path.GetDirectoryName(metadata)!;
            foreach (SnapshotFile file in snapshot.Files)
            {
                File.Delete(ResolveChildPath(directory, file.SnapshotFileName));
            }

            File.Delete(metadata);
            deleted++;
            if (!PathEquals(directory, RootDirectory) &&
                Directory.Exists(directory) &&
                !Directory.EnumerateFileSystemEntries(directory).Any())
            {
                Directory.Delete(directory);
            }
        }

        Directory.CreateDirectory(RootDirectory);
        return deleted;
    }

    public void PruneToRetentionLimit()
    {
        if (!Directory.Exists(RootDirectory))
        {
            return;
        }

        foreach (string directory in Directory.EnumerateFiles(RootDirectory, "*" + MetadataSuffix, SearchOption.AllDirectories)
                     .Select(Path.GetDirectoryName).OfType<string>().Distinct(StringComparer.OrdinalIgnoreCase))
        {
            EnforceRetention(directory);
        }
    }

    public RestoreResult RestoreSnapshot(Snapshot input)
    {
        ValidateSnapshot(input);
        Snapshot snapshot = AssessIntegrity(input);
        if (!snapshot.IsRestorable)
        {
            throw new InvalidOperationException(snapshot.IntegrityError);
        }

        string missionPath = NormalizeMissionPath(snapshot.MissionPath, false);
        Directory.CreateDirectory(Path.GetDirectoryName(missionPath)!);
        string recoveryName = $"{DateTime.Now:yyyy-MM-dd_HH-mm-ss}_{Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture)[..6]}";
        string recovery = Path.Combine(
            RootDirectory,
            "RecoveryBeforeRestore",
            BuildMissionKey(missionPath),
            recoveryName);
        Directory.CreateDirectory(recovery);
        List<string> current = EnumerateMissionFamily(missionPath);
        foreach (string source in current)
        {
            File.Copy(source, Path.Combine(recovery, Path.GetFileName(source)), false);
        }

        HashSet<string> expected = new(StringComparer.OrdinalIgnoreCase);
        List<(string Stage, string Destination)> staged = [];
        string operation = Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture);
        try
        {
            foreach (SnapshotFile file in snapshot.Files)
            {
                string destination = ResolveMissionCompanionPath(missionPath, file.OriginalFileName);
                string stage = destination + ".il2mec-restore-" + operation;
                File.Copy(ResolveChildPath(Path.GetDirectoryName(snapshot.MetadataPath)!, file.SnapshotFileName), stage, true);
                staged.Add((stage, destination));
                expected.Add(file.OriginalFileName);
            }

            foreach ((string stage, string destination) in staged)
            {
                File.Move(stage, destination, true);
            }

            foreach (string file in current.Where(file => !expected.Contains(Path.GetFileName(file))))
            {
                File.Delete(file);
            }

            return new RestoreResult(missionPath, recovery, staged.Count);
        }
        catch
        {
            foreach ((string stage, _) in staged)
            {
                TryDelete(stage);
            }

            foreach (string file in EnumerateMissionFamily(missionPath))
            {
                TryDelete(file);
            }

            foreach (string backup in Directory.EnumerateFiles(recovery))
            {
                File.Copy(backup, Path.Combine(Path.GetDirectoryName(missionPath)!, Path.GetFileName(backup)), true);
            }

            throw;
        }
    }

    public static List<string> EnumerateMissionFamily(string missionValue)
    {
        string missionPath = Path.GetFullPath(missionValue);
        string? directory = Path.GetDirectoryName(missionPath);
        if (directory is null || !Directory.Exists(directory))
        {
            return [];
        }

        string stem = Path.GetFileNameWithoutExtension(missionPath);
        return Directory.EnumerateFiles(directory)
            .Where(path => Path.GetFileNameWithoutExtension(path).Equals(stem, StringComparison.OrdinalIgnoreCase))
            .Select(Path.GetFullPath)
            .Order(StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    public static async Task WaitUntilMissionFamilyStableAsync(
        string missionValue,
        CancellationToken cancellationToken,
        TimeSpan? timeout = null,
        TimeSpan? sampleInterval = null,
        int requiredStableObservations = 4,
        TimeSpan? minimumWait = null)
    {
        string missionPath = NormalizeMissionPath(missionValue, true);
        TimeSpan effectiveTimeout = timeout ?? TimeSpan.FromSeconds(15);
        TimeSpan effectiveSample = sampleInterval ?? TimeSpan.FromMilliseconds(200);
        TimeSpan effectiveMinimum = minimumWait ?? TimeSpan.FromMilliseconds(800);
        if (effectiveTimeout <= TimeSpan.Zero || effectiveSample <= TimeSpan.Zero || requiredStableObservations < 2 || effectiveMinimum < TimeSpan.Zero || effectiveMinimum > effectiveTimeout)
        {
            throw new ArgumentOutOfRangeException(nameof(timeout), "Invalid mission stability timing.");
        }

        DateTime started = DateTime.UtcNow;
        List<(string Path, long Length, DateTime LastWrite)> previous = [];
        int stable = 0;
        while (DateTime.UtcNow - started <= effectiveTimeout)
        {
            cancellationToken.ThrowIfCancellationRequested();
            List<(string Path, long Length, DateTime LastWrite)> current = CaptureFamily(missionPath);
            stable = previous.Count > 0 && current.SequenceEqual(previous) ? stable + 1 : 1;
            if (DateTime.UtcNow - started >= effectiveMinimum && stable >= requiredStableObservations)
            {
                return;
            }

            previous = current;
            await Task.Delay(effectiveSample, cancellationToken).ConfigureAwait(false);
        }

        throw new TimeoutException("The Mission Editor did not finish writing the mission files before the recovery-point timeout.");
    }

    public static string Sha256File(string path)
    {
        using FileStream input = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(input));
    }

    public static string NormalizeMissionPath(string value, bool requireExists)
    {
        if (!Path.IsPathFullyQualified(value))
        {
            throw new ArgumentException("The mission path must be fully qualified.", nameof(value));
        }

        string path = Path.GetFullPath(value);
        if (!Path.GetExtension(path).Equals(".Mission", StringComparison.OrdinalIgnoreCase))
        {
            throw new ArgumentException("The autosave source must be an IL-2 .Mission file.", nameof(value));
        }

        if (requireExists && !File.Exists(path))
        {
            throw new FileNotFoundException("The mission file does not exist.", path);
        }

        return path;
    }

    private Snapshot ReadMetadata(string metadataPath)
    {
        string fullPath = Path.GetFullPath(metadataPath);
        EnsureInside(RootDirectory, fullPath, "Snapshot is outside the recovery folder.");
        Snapshot snapshot = JsonSerializer.Deserialize<Snapshot>(File.ReadAllText(fullPath, Encoding.UTF8), JsonOptions)
            ?? throw new JsonException("Autosave metadata root is invalid.");
        snapshot.MetadataPath = fullPath;
        ValidateSnapshot(snapshot);
        return snapshot;
    }

    private static void ValidateSnapshot(Snapshot snapshot)
    {
        if (snapshot.SchemaVersion != 1 || snapshot.Files.Count == 0 || !Path.IsPathFullyQualified(snapshot.MissionPath) ||
            string.IsNullOrWhiteSpace(snapshot.EditorProcessName) || !Path.IsPathFullyQualified(snapshot.EditorExecutablePath) || string.IsNullOrWhiteSpace(snapshot.MetadataPath))
        {
            throw new InvalidOperationException("Autosave metadata has an unsupported or incomplete format.");
        }

        HashSet<string> originals = new(StringComparer.OrdinalIgnoreCase);
        HashSet<string> stored = new(StringComparer.OrdinalIgnoreCase);
        foreach (SnapshotFile file in snapshot.Files)
        {
            _ = ResolveMissionCompanionPath(snapshot.MissionPath, file.OriginalFileName);
            _ = ResolveChildPath(Path.GetDirectoryName(snapshot.MetadataPath)!, file.SnapshotFileName);
            if (!originals.Add(file.OriginalFileName) || !stored.Add(file.SnapshotFileName) || file.Sha256.Length != 64)
            {
                throw new InvalidOperationException("Autosave metadata contains an invalid file entry.");
            }
        }

        if (!originals.Contains(Path.GetFileName(snapshot.MissionPath)))
        {
            throw new InvalidOperationException("Autosave metadata does not contain its .Mission file.");
        }
    }

    private static Snapshot AssessIntegrity(Snapshot snapshot)
    {
        try
        {
            foreach (SnapshotFile file in snapshot.Files)
            {
                string stored = ResolveChildPath(Path.GetDirectoryName(snapshot.MetadataPath)!, file.SnapshotFileName);
                if (!File.Exists(stored))
                {
                    snapshot.IntegrityError = "Missing file: " + file.OriginalFileName;
                    return snapshot;
                }

                if (new FileInfo(stored).Length != file.Length)
                {
                    snapshot.IntegrityError = "Incorrect file size: " + file.OriginalFileName;
                    return snapshot;
                }

                if (!Sha256File(stored).Equals(file.Sha256, StringComparison.OrdinalIgnoreCase))
                {
                    snapshot.IntegrityError = "Checksum mismatch: " + file.OriginalFileName;
                    return snapshot;
                }
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or CryptographicException)
        {
            snapshot.IntegrityError = "Could not verify snapshot: " + error.Message;
        }

        return snapshot;
    }

    private void EnforceRetention(string directory)
    {
        List<Snapshot> snapshots = [];
        if (!Directory.Exists(directory))
        {
            return;
        }

        foreach (string path in Directory.EnumerateFiles(directory, "*" + MetadataSuffix))
        {
            try
            {
                snapshots.Add(ReadMetadata(path));
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException)
            {
            }
        }

        foreach (Snapshot snapshot in snapshots.OrderByDescending(item => item.CreatedUtc).Skip(RetentionCount))
        {
            foreach (SnapshotFile file in snapshot.Files)
            {
                TryDelete(ResolveChildPath(directory, file.SnapshotFileName));
            }

            TryDelete(snapshot.MetadataPath);
        }
    }

    private static List<(string Path, long Length, DateTime LastWrite)> CaptureFamily(string missionPath)
    {
        List<string> files = EnumerateMissionFamily(missionPath);
        if (!files.Any(path => PathEquals(path, missionPath)))
        {
            throw new InvalidOperationException("The saved .Mission file disappeared before a recovery point could be created.");
        }

        return files.Select(path =>
        {
            FileInfo info = new(path);
            return (path, info.Length, info.LastWriteTimeUtc);
        }).ToList();
    }

    private static string BuildMissionKey(string missionPath)
    {
        string normalized = Path.GetFullPath(missionPath).ToUpperInvariant();
        string hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(normalized)));
        return SanitizeFileName(Path.GetFileNameWithoutExtension(missionPath)) + "_" + hash[..12];
    }

    private static string SanitizeFileName(string value)
    {
        HashSet<char> invalid = [.. Path.GetInvalidFileNameChars()];
        string cleaned = new string(value.Select(character => character < 32 || invalid.Contains(character) ? '_' : character).ToArray()).Trim().TrimEnd('.');
        if (string.IsNullOrEmpty(cleaned))
        {
            cleaned = "Mission";
        }

        return cleaned.Length > 80 ? cleaned[..80] : cleaned;
    }

    private static string ResolveChildPath(string directory, string fileName)
    {
        if (string.IsNullOrWhiteSpace(fileName) || Path.GetFileName(fileName) != fileName)
        {
            throw new InvalidOperationException("Autosave metadata contains an invalid stored filename.");
        }

        string path = Path.GetFullPath(Path.Combine(directory, fileName));
        EnsureInside(directory, path, "Autosave metadata points outside its snapshot directory.");
        return path;
    }

    private static string ResolveMissionCompanionPath(string missionPath, string fileName)
    {
        if (string.IsNullOrWhiteSpace(fileName) || Path.GetFileName(fileName) != fileName ||
            !Path.GetFileNameWithoutExtension(fileName).Equals(Path.GetFileNameWithoutExtension(missionPath), StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException("Autosave metadata contains an invalid mission companion filename.");
        }

        return Path.Combine(Path.GetDirectoryName(missionPath)!, fileName);
    }

    private static void EnsureInside(string directory, string path, string message)
    {
        string root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(directory)) + Path.DirectorySeparatorChar;
        if (!Path.GetFullPath(path).StartsWith(root, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException(message);
        }
    }

    private static void CopyFileAtomically(string source, string destination, bool replace)
    {
        string temporary = destination + ".tmp-" + Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture);
        try
        {
            File.Copy(source, temporary, false);
            File.Move(temporary, destination, replace);
        }
        catch
        {
            TryDelete(temporary);
            throw;
        }
    }

    private static void WriteUtf8Atomically(string path, string contents)
    {
        string temporary = path + ".tmp-" + Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture);
        try
        {
            File.WriteAllText(temporary, contents, new UTF8Encoding(false));
            File.Move(temporary, path, false);
        }
        catch
        {
            TryDelete(temporary);
            throw;
        }
    }

    private static bool PathEquals(string left, string right) =>
        Path.GetFullPath(left).Equals(Path.GetFullPath(right), StringComparison.OrdinalIgnoreCase);

    private static void TryDelete(string path)
    {
        try
        {
            File.Delete(path);
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }
}
