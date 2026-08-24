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
    Write(ini, "[AutoSave]\nEnabled=true\nGreatBattles=false\nKorea=true\nIntervalMinutes=7\nHistoricSnapshots=12\nTrayNotifications=false\n");
    auto options = il2mec::LoadAutoSaveOptions(ini);
    Require(options.enabled && !options.greatBattles && options.korea, "settings editor selection");
    Require(options.intervalMinutes == 7 && options.historicSnapshots == 12 && !options.trayNotifications, "settings values");
    Write(ini, "[AutoSave]\nIntervalMinutes=999\n");
    Require(il2mec::LoadAutoSaveOptions(ini) == il2mec::AutoSaveOptions{}, "invalid settings fallback");
}

void TestMissionTitle(const fs::path& root) {
    fs::path mission = fs::absolute(root / L"Title Mission.Mission");
    Write(mission, "mission");
    fs::path parsed;
    Require(il2mec::TryGetSavedMissionPath(L"IL2 Series Editor - " + mission.wstring() + L" *", parsed), "dirty mission title parse");
    Require(parsed == mission, "dirty mission path");
    fs::path binary = mission; binary.replace_extension(L".msnbin");
    Require(il2mec::TryGetSavedMissionPath(L"Editor - " + binary.wstring(), parsed), "msnbin title canonicalization");
    Require(parsed == mission, "msnbin canonical mission");
    Require(!il2mec::TryGetSavedMissionPath(L"Editor - <empty>", parsed), "empty mission rejection");
}

void TestStabilityAndSnapshots(const fs::path& root) {
    fs::path missions = root / L"Missions";
    fs::path mission = fs::absolute(missions / L"RecoveryTest.Mission");
    fs::path binary = missions / L"RecoveryTest.msnbin";
    fs::path localization = missions / L"RecoveryTest.eng";
    Write(mission, "version-one"); Write(binary, "compiled-one"); Write(localization, "english-one");

    il2mec::WaitUntilMissionFamilyStable(mission, 1s, 10ms, 3, 20ms);
    il2mec::SnapshotStore store(root / L"Autosave", 2);
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
    il2mec::SnapshotStore store(destination, 10);
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
        TestStabilityAndSnapshots(root);
        TestLegacyImport(root);
        wchar_t verifyExisting[8]{};
        if (GetEnvironmentVariableW(L"IL2MISSIONGUARD_VERIFY_DEFAULT_SNAPSHOTS", verifyExisting, static_cast<DWORD>(std::size(verifyExisting))) > 0) {
            il2mec::SnapshotStore existing;
            auto snapshots = existing.ListSnapshots(std::nullopt, 1);
            Require(!snapshots.empty(), "existing C# snapshot metadata compatibility");
        }
        fs::remove_all(root);
        std::wcout << L"All native autosave regression tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::wcerr << L"FAILED: " << il2mec::Utf8ToWide(error.what()) << L"\nTest data retained at " << root << L"\n";
        return 1;
    }
}
