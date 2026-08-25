#pragma once

#include "NativeCore.h"

#include <filesystem>

namespace il2mec {

inline constexpr wchar_t kCurrentVersion[] = L"1.4.0";

GitHubRelease FetchLatestGitHubRelease();
std::filesystem::path DownloadVerifiedUpdate(const GitHubRelease& release);
std::filesystem::path CurrentExecutablePath();
void LaunchSelfUpdate(
    const std::filesystem::path& downloadedExecutable,
    const std::filesystem::path& targetExecutable,
    DWORD processId);
int ApplyPendingUpdate(const std::filesystem::path& targetExecutable, DWORD processId);

}  // namespace il2mec
