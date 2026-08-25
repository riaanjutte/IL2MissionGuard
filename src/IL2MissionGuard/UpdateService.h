#pragma once

#include "NativeCore.h"

#include <filesystem>

namespace missionguard {

inline constexpr wchar_t kCurrentVersion[] = L"0.1.0-beta.1";

GitHubRelease FetchLatestGitHubRelease();
std::filesystem::path DownloadVerifiedUpdate(const GitHubRelease& release);
std::filesystem::path CurrentExecutablePath();
void LaunchSelfUpdate(
    const std::filesystem::path& downloadedExecutable,
    const std::filesystem::path& targetExecutable,
    DWORD processId);
int ApplyPendingUpdate(const std::filesystem::path& targetExecutable, DWORD processId);

}  // namespace missionguard
