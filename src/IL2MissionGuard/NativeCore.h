#pragma once

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace il2mec {

struct AutoSaveOptions {
    bool enabled = true;
    bool greatBattles = true;
    bool korea = true;
    int intervalMinutes = 5;
    int historicSnapshots = 10;
    bool trayNotifications = true;

    bool operator==(const AutoSaveOptions&) const = default;
};

struct SnapshotFile {
    std::wstring originalFileName;
    std::wstring snapshotFileName;
    std::uintmax_t length = 0;
    std::wstring sha256;
};

struct Snapshot {
    int schemaVersion = 1;
    std::filesystem::path missionPath;
    std::wstring editorProcessName;
    std::filesystem::path editorExecutablePath;
    std::chrono::system_clock::time_point createdUtc{};
    std::vector<SnapshotFile> files;
    std::filesystem::path metadataPath;
    std::wstring integrityError;

    [[nodiscard]] bool IsRestorable() const { return integrityError.empty(); }
};

struct RestoreResult {
    std::filesystem::path missionPath;
    std::filesystem::path safetyBackupDirectory;
    std::size_t restoredFileCount = 0;
};

struct SemanticVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;

    bool operator==(const SemanticVersion&) const = default;
};

struct GitHubRelease {
    std::wstring version;
    std::wstring releaseUrl;
    std::wstring assetUrl;
    std::wstring sha256;
};

std::filesystem::path LocalAppDataDirectory();
std::filesystem::path DefaultSettingsPath();
std::filesystem::path DefaultSnapshotRoot();
std::filesystem::path LegacySnapshotRoot();
std::filesystem::path DefaultLogPath();
AutoSaveOptions LoadAutoSaveOptions(const std::filesystem::path& settingsPath);
void SaveAutoSaveOptions(
    const std::filesystem::path& settingsPath,
    const AutoSaveOptions& options);

bool TryGetSavedMissionPath(
    const std::wstring& windowTitle,
    std::filesystem::path& missionPath,
    bool requireExists = true);
std::vector<std::filesystem::path> EnumerateMissionFamily(const std::filesystem::path& missionPath);
void WaitUntilMissionFamilyStable(
    const std::filesystem::path& missionPath,
    std::chrono::milliseconds timeout = std::chrono::seconds(15),
    std::chrono::milliseconds sampleInterval = std::chrono::milliseconds(200),
    int requiredStableObservations = 4,
    std::chrono::milliseconds minimumWait = std::chrono::milliseconds(800));
std::wstring Sha256File(const std::filesystem::path& path);
std::wstring FormatLocalSnapshotTime(std::chrono::system_clock::time_point value);
std::wstring FormatLocalMenuTime(std::chrono::system_clock::time_point value);
std::optional<SemanticVersion> ParseSemanticVersion(const std::wstring& value);
bool IsNewerVersion(const std::wstring& candidate, const std::wstring& current);
GitHubRelease ParseGitHubLatestReleaseJson(const std::string& json);

class SnapshotStore {
public:
    explicit SnapshotStore(
        std::filesystem::path rootDirectory = DefaultSnapshotRoot(),
        int retentionCount = 10);

    [[nodiscard]] const std::filesystem::path& RootDirectory() const { return rootDirectory_; }
    [[nodiscard]] int RetentionCount() const { return retentionCount_; }

    int ImportLegacySnapshots(const std::filesystem::path& legacyRootDirectory);
    Snapshot CreateSnapshot(
        const std::filesystem::path& missionPath,
        const std::wstring& editorProcessName,
        const std::filesystem::path& editorExecutablePath,
        std::optional<std::chrono::system_clock::time_point> createdUtc = std::nullopt);
    std::vector<Snapshot> ListSnapshots(
        const std::optional<std::filesystem::path>& missionPath = std::nullopt,
        int maximum = INT_MAX) const;
    void PruneToRetentionLimit();
    RestoreResult RestoreSnapshot(const Snapshot& snapshot) const;

private:
    std::filesystem::path rootDirectory_;
    int retentionCount_;

    Snapshot ReadMetadata(const std::filesystem::path& metadataPath) const;
    Snapshot AssessIntegrity(Snapshot snapshot) const;
    void ValidateSnapshot(const Snapshot& snapshot) const;
    void EnforceRetention(const std::filesystem::path& snapshotDirectory) const;
};

std::string WideToUtf8(const std::wstring& value);
std::wstring Utf8ToWide(const std::string& value);
std::wstring Win32ErrorMessage(DWORD error = GetLastError());

}  // namespace il2mec
