#include "UpdateService.h"

#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace missionguard {
namespace {

constexpr wchar_t kLatestReleaseApi[] = L"https://api.github.com/repos/riaanjutte/IL2MissionGuard/releases?per_page=20";
constexpr std::size_t kMaximumReleaseResponse = 4 * 1024 * 1024;
constexpr std::size_t kMaximumExecutableSize = 256 * 1024 * 1024;

struct InternetCloser {
    void operator()(void* value) const { if (value) WinHttpCloseHandle(value); }
};
using InternetHandle = std::unique_ptr<void, InternetCloser>;

std::vector<unsigned char> HttpGet(const std::wstring& url, const wchar_t* headers, std::size_t maximumSize) {
    URL_COMPONENTS components{sizeof(components)};
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components) || components.nScheme != INTERNET_SCHEME_HTTPS) {
        throw std::runtime_error("The update service returned an invalid HTTPS URL.");
    }
    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.lpszExtraInfo && components.dwExtraInfoLength) path.append(components.lpszExtraInfo, components.dwExtraInfoLength);

    InternetHandle session(WinHttpOpen(L"IL2MissionGuard/0.1.0-beta.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) throw std::runtime_error("Windows could not initialize the update connection.");
    WinHttpSetTimeouts(session.get(), 5000, 5000, 10000, 10000);
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(session.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
    if (!connection) throw std::runtime_error("Windows could not connect to GitHub.");
    InternetHandle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) throw std::runtime_error("Windows could not create the GitHub update request.");
    if (!WinHttpSendRequest(request.get(), headers, headers ? static_cast<DWORD>(-1) : 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.get(), nullptr)) {
        throw std::runtime_error("The GitHub update request failed: " + WideToUtf8(Win32ErrorMessage()));
    }
    DWORD status = 0, statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) || status != 200) {
        throw std::runtime_error("GitHub returned HTTP " + std::to_string(status) + " while checking for updates.");
    }

    std::vector<unsigned char> result;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) throw std::runtime_error("The GitHub response could not be read.");
        if (!available) break;
        if (available > maximumSize || result.size() > maximumSize - available) throw std::runtime_error("The GitHub update response is unexpectedly large.");
        const std::size_t offset = result.size();
        result.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), result.data() + offset, available, &read)) throw std::runtime_error("The GitHub response could not be read.");
        result.resize(offset + read);
    }
    return result;
}

std::wstring Lower(std::wstring value) {
    if (!value.empty()) CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}

std::wstring Quote(const fs::path& value) {
    return L"\"" + value.wstring() + L"\"";
}

}  // namespace

GitHubRelease FetchLatestGitHubRelease() {
    constexpr wchar_t headers[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    const auto bytes = HttpGet(kLatestReleaseApi, headers, kMaximumReleaseResponse);
    return ParseGitHubLatestReleaseJson(std::string(bytes.begin(), bytes.end()));
}

fs::path DownloadVerifiedUpdate(const GitHubRelease& release) {
    if (!IsNewerVersion(release.version, kCurrentVersion)) throw std::runtime_error("The selected release is not newer than this version.");
    const auto bytes = HttpGet(release.assetUrl, L"Accept: application/octet-stream\r\n", kMaximumExecutableSize);
    if (bytes.size() < 64 * 1024 || bytes[0] != 'M' || bytes[1] != 'Z') throw std::runtime_error("The downloaded update is not a valid Windows executable.");

    fs::path directory = LocalAppDataDirectory() / L"Updates";
    fs::create_directories(directory);
    fs::path destination = directory / (L"IL2MissionGuard-" + release.version + L".exe");
    fs::path temporary = destination.wstring() + L".part";
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("The downloaded update could not be saved.");
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) throw std::runtime_error("The downloaded update could not be saved.");
        if (Lower(Sha256File(temporary)) != Lower(release.sha256)) throw std::runtime_error("The downloaded update failed SHA-256 verification.");
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("The verified update could not be finalized: " + WideToUtf8(Win32ErrorMessage()));
        }
        return destination;
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

fs::path CurrentExecutablePath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) throw std::runtime_error("Windows could not locate the running Mission Guard executable.");
    return fs::absolute(fs::path(std::wstring(buffer.data(), length))).lexically_normal();
}

void LaunchSelfUpdate(const fs::path& downloadedExecutable, const fs::path& targetExecutable, DWORD processId) {
    if (!downloadedExecutable.is_absolute() || !targetExecutable.is_absolute() || !fs::is_regular_file(downloadedExecutable)) {
        throw std::runtime_error("The verified update path is invalid.");
    }
    std::wstring command = Quote(downloadedExecutable) + L" --apply-update " + Quote(targetExecutable) + L" " + std::to_wstring(processId);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(downloadedExecutable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        downloadedExecutable.parent_path().c_str(), &startup, &process)) {
        throw std::runtime_error("Windows could not start the verified updater: " + WideToUtf8(Win32ErrorMessage()));
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

int ApplyPendingUpdate(const fs::path& targetValue, DWORD processId) {
    const fs::path source = CurrentExecutablePath();
    const fs::path target = fs::absolute(targetValue).lexically_normal();
    if (!target.is_absolute() || target.extension() != L".exe" || source == target) throw std::runtime_error("The update target is invalid.");

    if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId)) {
        const DWORD wait = WaitForSingleObject(process, 30000);
        CloseHandle(process);
        if (wait != WAIT_OBJECT_0) throw std::runtime_error("The previous Mission Guard process did not close in time.");
    }

    const fs::path staged = target.wstring() + L".update-new";
    const fs::path backup = target.wstring() + L".update-backup";
    std::error_code ignored;
    fs::remove(staged, ignored);
    fs::remove(backup, ignored);
    bool copied = false;
    DWORD copyError = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 20 && !copied; ++attempt) {
        copied = CopyFileW(source.c_str(), staged.c_str(), FALSE) != FALSE;
        if (!copied) { copyError = GetLastError(); std::this_thread::sleep_for(250ms); }
    }
    if (!copied) throw std::runtime_error("The Mission Guard update could not be staged: " + WideToUtf8(Win32ErrorMessage(copyError)));
    try {
        if (Lower(Sha256File(source)) != Lower(Sha256File(staged))) throw std::runtime_error("The staged update failed verification.");
        if (fs::is_regular_file(target) && !CopyFileW(target.c_str(), backup.c_str(), FALSE)) {
            throw std::runtime_error("The existing Mission Guard executable could not be backed up: " + WideToUtf8(Win32ErrorMessage()));
        }
        if (!MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("The Mission Guard executable could not be replaced: " + WideToUtf8(Win32ErrorMessage()));
        }
    } catch (...) {
        fs::remove(staged, ignored);
        fs::remove(backup, ignored);
        throw;
    }
    if (Lower(Sha256File(source)) != Lower(Sha256File(target))) {
        if (fs::is_regular_file(backup)) MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        throw std::runtime_error("The installed update failed verification; the previous executable was restored.");
    }
    fs::remove(backup, ignored);

    std::wstring command = Quote(target);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(target.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        target.parent_path().c_str(), &startup, &process)) {
        throw std::runtime_error("The update was installed, but Mission Guard could not be restarted: " + WideToUtf8(Win32ErrorMessage()));
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    MoveFileExW(source.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return 0;
}

}  // namespace missionguard
