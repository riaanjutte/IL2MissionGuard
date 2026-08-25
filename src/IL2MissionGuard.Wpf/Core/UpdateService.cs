using System.Diagnostics;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text.Json;

namespace IL2MissionGuard.Core;

internal static class UpdateService
{
    public const string CurrentVersion = "0.1.0-beta.1";
    private const string LatestReleaseApi = "https://api.github.com/repos/riaanjutte/IL2MissionGuard/releases?per_page=20";
    private const string ReleasePrefix = "https://github.com/riaanjutte/IL2MissionGuard/releases/";
    private const string AssetPrefix = "https://github.com/riaanjutte/IL2MissionGuard/releases/download/";
    private const int MaximumReleaseBytes = 4 * 1024 * 1024;
    private const int MaximumExecutableBytes = 256 * 1024 * 1024;

    public static bool IsNewerVersion(string candidate, string current)
    {
        SemanticVersion left = ParseVersion(candidate);
        SemanticVersion right = ParseVersion(current);
        int coreComparison = left.Major != right.Major ? left.Major.CompareTo(right.Major)
            : left.Minor != right.Minor ? left.Minor.CompareTo(right.Minor)
            : left.Patch.CompareTo(right.Patch);
        if (coreComparison != 0)
        {
            return coreComparison > 0;
        }

        if (left.Prerelease.Length == 0 || right.Prerelease.Length == 0)
        {
            return left.Prerelease.Length == 0 && right.Prerelease.Length != 0;
        }

        int identifiers = Math.Min(left.Prerelease.Length, right.Prerelease.Length);
        for (int index = 0; index < identifiers; index++)
        {
            string leftPart = left.Prerelease[index];
            string rightPart = right.Prerelease[index];
            bool leftNumeric = leftPart.All(char.IsAsciiDigit);
            bool rightNumeric = rightPart.All(char.IsAsciiDigit);
            if (leftNumeric != rightNumeric)
            {
                return !leftNumeric;
            }

            int comparison = leftNumeric && leftPart.Length != rightPart.Length
                ? leftPart.Length.CompareTo(rightPart.Length)
                : string.CompareOrdinal(leftPart, rightPart);
            if (comparison != 0)
            {
                return comparison > 0;
            }
        }

        return left.Prerelease.Length > right.Prerelease.Length;
    }

    public static GitHubRelease ParseLatestRelease(string json)
    {
        using JsonDocument document = JsonDocument.Parse(json);
        JsonElement root = document.RootElement;
        JsonElement releaseJson = root;
        if (root.ValueKind == JsonValueKind.Array)
        {
            JsonElement.ArrayEnumerator releases = root.EnumerateArray();
            if (!releases.MoveNext())
            {
                throw new InvalidOperationException("GitHub has no published releases.");
            }

            releaseJson = releases.Current;
        }

        if (releaseJson.ValueKind != JsonValueKind.Object)
        {
            throw new InvalidOperationException("The GitHub release response is not an object.");
        }

        string version = releaseJson.GetProperty("tag_name").GetString() ?? throw new InvalidOperationException("The latest GitHub release has no version tag.");
        _ = ParseVersion(version);
        string releaseUrl = releaseJson.GetProperty("html_url").GetString() ?? string.Empty;
        if (!releaseUrl.StartsWith(ReleasePrefix, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException("The GitHub release URL is not trusted.");
        }

        foreach (JsonElement asset in releaseJson.GetProperty("assets").EnumerateArray())
        {
            if (!string.Equals(asset.GetProperty("name").GetString(), "IL2MissionGuard.exe", StringComparison.Ordinal))
            {
                continue;
            }

            string assetUrl = asset.GetProperty("browser_download_url").GetString() ?? string.Empty;
            string digest = asset.GetProperty("digest").GetString() ?? string.Empty;
            if (!assetUrl.StartsWith(AssetPrefix, StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidOperationException("The update asset URL is not trusted.");
            }

            if (!digest.StartsWith("sha256:", StringComparison.OrdinalIgnoreCase) || !IsSha256(digest[7..]))
            {
                throw new InvalidOperationException("The update asset has no valid SHA-256 digest.");
            }

            return new GitHubRelease(version, releaseUrl, assetUrl, digest[7..].ToLowerInvariant());
        }

        throw new InvalidOperationException("The latest GitHub release does not contain IL2MissionGuard.exe.");
    }

    public static async Task<GitHubRelease> FetchLatestAsync(CancellationToken cancellationToken = default)
    {
        using HttpClient client = CreateClient();
        using HttpResponseMessage response = await client.GetAsync(LatestReleaseApi, HttpCompletionOption.ResponseHeadersRead, cancellationToken).ConfigureAwait(false);
        response.EnsureSuccessStatusCode();
        byte[] contents = await ReadLimitedAsync(response.Content, MaximumReleaseBytes, cancellationToken).ConfigureAwait(false);
        return ParseLatestRelease(System.Text.Encoding.UTF8.GetString(contents));
    }

    public static async Task<string> DownloadVerifiedAsync(GitHubRelease release, CancellationToken cancellationToken = default)
    {
        if (!IsNewerVersion(release.Version, CurrentVersion))
        {
            throw new InvalidOperationException("The selected release is not newer than this version.");
        }

        using HttpClient client = CreateClient();
        using HttpResponseMessage response = await client.GetAsync(release.AssetUrl, HttpCompletionOption.ResponseHeadersRead, cancellationToken).ConfigureAwait(false);
        response.EnsureSuccessStatusCode();
        byte[] contents = await ReadLimitedAsync(response.Content, MaximumExecutableBytes, cancellationToken).ConfigureAwait(false);
        if (contents.Length < 64 * 1024 || contents[0] != 'M' || contents[1] != 'Z')
        {
            throw new InvalidOperationException("The downloaded update is not a valid Windows executable.");
        }

        string directory = Path.Combine(SettingsStore.LocalAppDataDirectory, "Updates");
        Directory.CreateDirectory(directory);
        string destination = Path.Combine(directory, $"IL2MissionGuard-{release.Version}.exe");
        string temporary = destination + ".part";
        try
        {
            await File.WriteAllBytesAsync(temporary, contents, cancellationToken).ConfigureAwait(false);
            string digest = Convert.ToHexString(SHA256.HashData(contents));
            if (!digest.Equals(release.Sha256, StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidOperationException("The downloaded update failed SHA-256 verification.");
            }

            File.Move(temporary, destination, true);
            return destination;
        }
        catch
        {
            File.Delete(temporary);
            throw;
        }
    }

    public static void LaunchSelfUpdate(string downloadedExecutable)
    {
        string target = Environment.ProcessPath ?? throw new InvalidOperationException("Windows could not locate the running Mission Guard executable.");
        _ = Process.Start(new ProcessStartInfo(downloadedExecutable)
        {
            UseShellExecute = false,
            ArgumentList = { "--apply-update", target, Environment.ProcessId.ToString(System.Globalization.CultureInfo.InvariantCulture) },
            WorkingDirectory = Path.GetDirectoryName(downloadedExecutable),
        }) ?? throw new InvalidOperationException("Windows could not start the verified updater.");
    }

    public static async Task<int> ApplyPendingUpdateAsync(string targetValue, int processId)
    {
        string source = Environment.ProcessPath ?? throw new InvalidOperationException("Windows could not locate the update executable.");
        string target = Path.GetFullPath(targetValue);
        if (!Path.GetExtension(target).Equals(".exe", StringComparison.OrdinalIgnoreCase) || PathEquals(source, target))
        {
            throw new InvalidOperationException("The update target is invalid.");
        }

        try
        {
            using Process process = Process.GetProcessById(processId);
            using CancellationTokenSource timeout = new(TimeSpan.FromSeconds(30));
            await process.WaitForExitAsync(timeout.Token).ConfigureAwait(false);
        }
        catch (ArgumentException)
        {
        }
        catch (OperationCanceledException)
        {
            throw new TimeoutException("The previous Mission Guard process did not close in time.");
        }

        string staged = target + ".update-new";
        string backup = target + ".update-backup";
        File.Delete(staged);
        File.Delete(backup);
        Exception? copyError = null;
        for (int attempt = 0; attempt < 20; attempt++)
        {
            try
            {
                File.Copy(source, staged, true);
                copyError = null;
                break;
            }
            catch (IOException error)
            {
                copyError = error;
                await Task.Delay(250).ConfigureAwait(false);
            }
        }

        if (copyError is not null)
        {
            throw new IOException("The Mission Guard update could not be staged.", copyError);
        }

        if (!SnapshotStore.Sha256File(source).Equals(SnapshotStore.Sha256File(staged), StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException("The staged update failed verification.");
        }

        if (File.Exists(target))
        {
            File.Copy(target, backup, true);
        }

        try
        {
            File.Move(staged, target, true);
            if (!SnapshotStore.Sha256File(source).Equals(SnapshotStore.Sha256File(target), StringComparison.OrdinalIgnoreCase))
            {
                if (File.Exists(backup))
                {
                    File.Move(backup, target, true);
                }

                throw new InvalidOperationException("The installed update failed verification; the previous executable was restored.");
            }

            File.Delete(backup);
            _ = Process.Start(new ProcessStartInfo(target) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(target) });
            return 0;
        }
        catch
        {
            if (File.Exists(backup))
            {
                File.Move(backup, target, true);
            }

            throw;
        }
    }

    private static HttpClient CreateClient()
    {
        HttpClient client = new() { Timeout = TimeSpan.FromSeconds(45) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("IL2MissionGuard/" + CurrentVersion);
        client.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        client.DefaultRequestHeaders.Add("X-GitHub-Api-Version", "2022-11-28");
        return client;
    }

    private static async Task<byte[]> ReadLimitedAsync(HttpContent content, int maximum, CancellationToken cancellationToken)
    {
        if (content.Headers.ContentLength > maximum)
        {
            throw new InvalidOperationException("The GitHub response is unexpectedly large.");
        }

        await using Stream input = await content.ReadAsStreamAsync(cancellationToken).ConfigureAwait(false);
        using MemoryStream output = new();
        byte[] buffer = new byte[81_920];
        int read;
        while ((read = await input.ReadAsync(buffer, cancellationToken).ConfigureAwait(false)) > 0)
        {
            if (output.Length + read > maximum)
            {
                throw new InvalidOperationException("The GitHub response is unexpectedly large.");
            }

            output.Write(buffer, 0, read);
        }

        return output.ToArray();
    }

    private static SemanticVersion ParseVersion(string value)
    {
        string text = value.Trim().TrimStart('v', 'V');
        string[] versionAndPrerelease = text.Split('-', 2);
        string[] core = versionAndPrerelease[0].Split('.');
        if (core.Length != 3 || core.Any(part => !int.TryParse(part, out _) || (part.Length > 1 && part[0] == '0')))
        {
            throw new ArgumentException("A release version is not valid semantic version text.", nameof(value));
        }

        string[] prerelease = versionAndPrerelease.Length == 1 ? [] : versionAndPrerelease[1].Split('.');
        if (prerelease.Any(part => part.Length == 0 || part.Any(character => !char.IsAsciiLetterOrDigit(character) && character != '-') ||
                                   (part.Length > 1 && part[0] == '0' && part.All(char.IsAsciiDigit))))
        {
            throw new ArgumentException("A release version is not valid semantic version text.", nameof(value));
        }

        return new SemanticVersion(int.Parse(core[0]), int.Parse(core[1]), int.Parse(core[2]), prerelease);
    }

    private sealed record SemanticVersion(int Major, int Minor, int Patch, string[] Prerelease);

    private static bool IsSha256(string value) => value.Length == 64 && value.All(Uri.IsHexDigit);

    private static bool PathEquals(string left, string right) =>
        Path.GetFullPath(left).Equals(Path.GetFullPath(right), StringComparison.OrdinalIgnoreCase);
}
