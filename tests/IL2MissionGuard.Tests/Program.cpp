#include "../../src/IL2MissionGuard/NativeCore.h"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void Write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value;
}

std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

fs::path NewTestRoot() {
    wchar_t temp[MAX_PATH + 1]{};
    GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    fs::path root = fs::path(temp) / (L"IL2MissionGuard-Tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root);
    return root;
}

void TestSettings(const fs::path& root) {
    fs::path ini = root / L"settings.ini";
    Write(ini, "[AutoSave]\nEnabled=true\nGreatBattles=false\nKorea=true\nIntervalMinutes=7\nHistoricSnapshots=12\nTrayNotifications=false\nTheme=Dark\n");
    auto options = missionguard::LoadAutoSaveOptions(ini);
    Require(options.enabled && !options.greatBattles && options.korea, "settings editor selection");
    Require(options.intervalMinutes == 7 && options.historicSnapshots == 12 && !options.trayNotifications &&
            options.theme == missionguard::ThemeMode::Dark, "settings values");
    Write(ini, "[AutoSave]\nIntervalMinutes=999\n");
    Require(missionguard::LoadAutoSaveOptions(ini) == missionguard::AutoSaveOptions{}, "invalid settings fallback");

    Write(ini,
          "[EditorDimensions]\nMissionTreeWidth=333\n\n"
          "[KoreaOptions]\nForceViewportToSdr=true\n\n"
          "[AutoSave]\nEnabled=true\nIntervalMinutes=5\n");
    const missionguard::AutoSaveOptions requested{true, false, true, 17, 42, false, missionguard::ThemeMode::Light};
    missionguard::SaveAutoSaveOptions(ini, requested);
    Require(missionguard::LoadAutoSaveOptions(ini) == requested, "saved autosave settings round trip");
    const std::string updated = Read(ini);
    Require(updated.find("MissionTreeWidth=333") != std::string::npos, "dimension section preserved");
    Require(updated.find("ForceViewportToSdr=true") != std::string::npos, "Korea section preserved");
    Require(updated.find("Theme=Light") != std::string::npos, "theme saved");

    bool invalidRejected = false;
    try {
        missionguard::SaveAutoSaveOptions(ini, missionguard::AutoSaveOptions{true, false, false, 5, 10, true, missionguard::ThemeMode::System});
    } catch (const std::invalid_argument&) {
        invalidRejected = true;
    }
    Require(invalidRejected, "invalid saved settings rejected");
    Require(missionguard::LoadAutoSaveOptions(ini) == requested, "invalid save left settings unchanged");

    const missionguard::AutoSaveOptions bothEditorsOff{false, false, false, 5, 10, true, missionguard::ThemeMode::System};
    missionguard::SaveAutoSaveOptions(ini, bothEditorsOff);
    Require(missionguard::LoadAutoSaveOptions(ini) == bothEditorsOff, "both editor autosave toggles can be disabled");
}

void TestMissionTitle(const fs::path& root) {
    fs::path mission = fs::absolute(root / L"Title Mission.Mission");
    Write(mission, "mission");
    fs::path parsed;
    Require(missionguard::TryGetSavedMissionPath(L"IL2 Series Editor - " + mission.wstring() + L" *", parsed), "dirty mission title parse");
    Require(parsed == mission, "dirty mission path");
    fs::path binary = mission; binary.replace_extension(L".msnbin");
    Require(missionguard::TryGetSavedMissionPath(L"Editor - " + binary.wstring(), parsed), "msnbin title canonicalization");
    Require(parsed == mission, "msnbin canonical mission");
    Require(!missionguard::TryGetSavedMissionPath(L"Editor - <empty>", parsed), "empty mission rejection");
}

void TestUpdateMetadata() {
    Require(missionguard::ParseSemanticVersion(L"v1.2.3") == missionguard::SemanticVersion{1, 2, 3}, "semantic version parse");
    Require(missionguard::ParseSemanticVersion(L"v0.1.0-beta.1") == missionguard::SemanticVersion{0, 1, 0, L"beta.1"}, "prerelease version parse");
    Require(!missionguard::ParseSemanticVersion(L"1.2") && !missionguard::ParseSemanticVersion(L"1.2.3-beta..1") &&
            !missionguard::ParseSemanticVersion(L"1.2.3-beta.") && !missionguard::ParseSemanticVersion(L"01.2.3"), "invalid semantic versions rejected");
    Require(missionguard::IsNewerVersion(L"v2.0.0", L"1.99.99"), "major update comparison");
    Require(missionguard::IsNewerVersion(L"1.2.1", L"1.2.0") && !missionguard::IsNewerVersion(L"1.2.0", L"1.2.0"), "patch update comparison");
    Require(missionguard::IsNewerVersion(L"0.1.0-beta.2", L"0.1.0-beta.1") &&
            missionguard::IsNewerVersion(L"0.1.0", L"0.1.0-beta.2") &&
            !missionguard::IsNewerVersion(L"0.1.0-alpha.1", L"0.1.0-beta.1"), "prerelease update comparison");

    const std::string json = R"([{
        "tag_name":"v0.1.0-beta.2",
        "html_url":"https://github.com/riaanjutte/IL2MissionGuard/releases/tag/v0.1.0-beta.2",
        "assets":[{
            "name":"IL2MissionGuard.exe",
            "browser_download_url":"https://github.com/riaanjutte/IL2MissionGuard/releases/download/v0.1.0-beta.2/IL2MissionGuard.exe",
            "digest":"sha256:d62319688f4f86f4d70a555e794eb5f673fa6ef1fadb6a48780b0d413b171b19"
        }]
    }])";
    const auto release = missionguard::ParseGitHubLatestReleaseJson(json);
    Require(release.version == L"v0.1.0-beta.2" && release.sha256 == L"d62319688f4f86f4d70a555e794eb5f673fa6ef1fadb6a48780b0d413b171b19", "GitHub release parse");

    bool untrustedRejected = false;
    try {
        std::string untrusted = json;
        const auto position = untrusted.find("https://github.com/riaanjutte/IL2MissionGuard/releases/download/");
        untrusted.replace(position, std::string("https://github.com/riaanjutte/IL2MissionGuard/releases/download/").size(), "https://example.com/");
        (void)missionguard::ParseGitHubLatestReleaseJson(untrusted);
    } catch (...) { untrustedRejected = true; }
    Require(untrustedRejected, "untrusted update URL rejected");
}

void TestStabilityAndSnapshots(const fs::path& root) {
    fs::path missions = root / L"Missions";
    fs::path mission = fs::absolute(missions / L"RecoveryTest.Mission");
    fs::path binary = missions / L"RecoveryTest.msnbin";
    fs::path localization = missions / L"RecoveryTest.eng";
    Write(mission, "version-one"); Write(binary, "compiled-one"); Write(localization, "english-one");

    missionguard::WaitUntilMissionFamilyStable(mission, 1s, 10ms, 3, 20ms);
    missionguard::SnapshotStore store(root / L"Autosave", 2);
    Require(store.CountSnapshots() == 0, "empty snapshot count");
    fs::path fakeEditor = fs::absolute(root / L"IL2Editor.exe"); Write(fakeEditor, "fake");
    auto firstTime = std::chrono::system_clock::now() - 3s;
    auto first = store.CreateSnapshot(mission, L"IL2Editor", fakeEditor, firstTime);
    Require(first.files.size() == 3, "complete mission family snapshot");
    Require(fs::exists(first.metadataPath), "snapshot metadata created");
    std::string metadata = Read(first.metadataPath);
    Require(metadata.find("\"SchemaVersion\": 1") != std::string::npos && metadata.find("\"CreatedUtc\"") != std::string::npos, "C# metadata schema compatibility");

    Write(mission, "version-two"); Write(binary, "compiled-two");
    auto second = store.CreateSnapshot(mission, L"IL2Editor", fakeEditor, std::chrono::system_clock::now() - 2s);
    Write(mission, "version-three");
    auto third = store.CreateSnapshot(mission, L"IL2Editor", fakeEditor, std::chrono::system_clock::now() - 1s);
    auto retained = store.ListSnapshots(mission, 10);
    Require(retained.size() == 2, "per-mission retention");
    Require(store.CountSnapshots() == 2, "fast snapshot count follows retention");
    Require(retained[0].metadataPath == third.metadataPath && retained[1].metadataPath == second.metadataPath, "newest snapshots retained");

    Write(mission, "unwanted-current"); Write(binary, "unwanted-compiled"); Write(localization, "unwanted-localization");
    auto restored = store.RestoreSnapshot(second);
    Require(Read(mission) == "version-two" && Read(binary) == "compiled-two" && Read(localization) == "english-one", "mission family restored");
    Require(fs::is_directory(restored.safetyBackupDirectory), "pre-restore safety backup");
    Require(Read(restored.safetyBackupDirectory / mission.filename()) == "unwanted-current", "safety backup contents");

    auto damaged = store.ListSnapshots(mission, 10);
    Require(!damaged.empty(), "snapshots remain listed");
    const auto& damageTarget = damaged.front();
    Write(damageTarget.metadataPath.parent_path() / damageTarget.files.front().snapshotFileName, "tampered");
    auto afterDamage = store.ListSnapshots(mission, 10);
    Require(!afterDamage.empty() && !afterDamage.front().IsRestorable(), "checksum damage detected and listed");
    bool rejected = false;
    try { store.RestoreSnapshot(afterDamage.front()); } catch (...) { rejected = true; }
    Require(rejected, "damaged restore rejected");
}

void TestLegacyImport(const fs::path& root) {
    fs::path legacy = root / L"Legacy";
    fs::path destination = root / L"Imported";
    Write(legacy / L"Mission_ABC" / L"old.Mission", "old");
    Write(legacy / L"Mission_ABC" / L"ignored.tmp-123", "temporary");
    missionguard::SnapshotStore store(destination, 10);
    Require(store.ImportLegacySnapshots(legacy) == 1, "legacy import count");
    Require(fs::exists(destination / L"Mission_ABC" / L"old.Mission"), "legacy file imported");
    Require(fs::exists(legacy / L"Mission_ABC" / L"old.Mission"), "legacy source retained");
    Require(store.ImportLegacySnapshots(legacy) == 0, "legacy import idempotence");
}

}  // namespace

int wmain() {
    fs::path root = NewTestRoot();
    try {
        TestSettings(root);
        TestMissionTitle(root);
        TestUpdateMetadata();
        TestStabilityAndSnapshots(root);
        TestLegacyImport(root);
        wchar_t verifyExisting[8]{};
        if (GetEnvironmentVariableW(L"IL2MISSIONGUARD_VERIFY_DEFAULT_SNAPSHOTS", verifyExisting, static_cast<DWORD>(std::size(verifyExisting))) > 0) {
            missionguard::SnapshotStore existing;
            auto snapshots = existing.ListSnapshots(std::nullopt, 1);
            Require(!snapshots.empty(), "existing C# snapshot metadata compatibility");
        }
        fs::remove_all(root);
        std::wcout << L"All native autosave regression tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::wcerr << L"FAILED: " << missionguard::Utf8ToWide(error.what()) << L"\nTest data retained at " << root << L"\n";
        return 1;
    }
}
