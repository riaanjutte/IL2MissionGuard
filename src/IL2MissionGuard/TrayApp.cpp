#include "NativeCore.h"
#include "resource.h"

#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

constexpr wchar_t kWindowClass[] = L"IL2MissionGuard.Window";
// Retained so IL2MEC can upgrade and stop existing installations safely.
constexpr wchar_t kMutexName[] = L"Local\\IL2MEC.AutoSave.Agent";
constexpr wchar_t kStopEventName[] = L"Local\\IL2MEC.AutoSave.Stop";
constexpr UINT kTrayCallback = WM_APP + 1;
constexpr UINT kOpenSettingsMessage = WM_APP + 2;
constexpr UINT kTimerId = 1;
constexpr UINT kSaveCommandId = 0x8037;
constexpr UINT kMenuSave = 1001;
constexpr UINT kMenuFolder = 1002;
constexpr UINT kMenuLog = 1003;
constexpr UINT kMenuExit = 1004;
constexpr UINT kMenuSettings = 1005;
constexpr UINT kRestoreFirst = 2000;
constexpr UINT kRestoreLast = 2199;

struct HandleCloser { void operator()(HANDLE value) const { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct ProcessInfo {
    DWORD id = 0;
    std::wstring name;
    fs::path executable;
    HWND window = nullptr;
    std::wstring title;
};

struct SettingsDialogState {
    il2mec::AutoSaveOptions options;
    fs::path settingsPath;
    HINSTANCE instance = nullptr;
    bool saved = false;
};

void UpdateSettingsControlState(HWND dialog) {
    const bool enabled = IsDlgButtonChecked(dialog, IDC_AUTOSAVE_ENABLED) == BST_CHECKED;
    for (int controlId : {IDC_GREAT_BATTLES, IDC_KOREA, IDC_INTERVAL, IDC_SNAPSHOTS, IDC_NOTIFICATIONS}) {
        EnableWindow(GetDlgItem(dialog, controlId), enabled);
    }
}

void CenterDialog(HWND dialog) {
    RECT bounds{};
    if (!GetWindowRect(dialog, &bounds)) return;
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    const int x = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
    const int y = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
    SetWindowPos(dialog, nullptr, x, y, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
}

INT_PTR CALLBACK SettingsDialogProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<SettingsDialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<SettingsDialogState*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        SendMessageW(dialog, WM_SETICON, ICON_SMALL,
                     reinterpret_cast<LPARAM>(LoadIconW(state->instance, MAKEINTRESOURCEW(IDI_APP_ICON))));
        CheckDlgButton(dialog, IDC_AUTOSAVE_ENABLED, state->options.enabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_GREAT_BATTLES, state->options.greatBattles ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_KOREA, state->options.korea ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_NOTIFICATIONS, state->options.trayNotifications ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(dialog, IDC_INTERVAL, static_cast<UINT>(state->options.intervalMinutes), FALSE);
        SetDlgItemInt(dialog, IDC_SNAPSHOTS, static_cast<UINT>(state->options.historicSnapshots), FALSE);
        UpdateSettingsControlState(dialog);
        CenterDialog(dialog);
        return TRUE;
    }
    if (!state) return FALSE;

    if (message == WM_CTLCOLORSTATIC && GetDlgCtrlID(reinterpret_cast<HWND>(lParam)) == IDC_NEW_MISSION_NOTE) {
        HDC device = reinterpret_cast<HDC>(wParam);
        SetTextColor(device, RGB(190, 130, 0));
        SetBkMode(device, TRANSPARENT);
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
    }
    if (message != WM_COMMAND) return FALSE;

    const WORD command = LOWORD(wParam);
    if (command == IDC_AUTOSAVE_ENABLED && HIWORD(wParam) == BN_CLICKED) {
        UpdateSettingsControlState(dialog);
        return TRUE;
    }
    if (command == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (command != IDOK) return FALSE;

    BOOL intervalValid = FALSE, snapshotsValid = FALSE;
    const UINT interval = GetDlgItemInt(dialog, IDC_INTERVAL, &intervalValid, FALSE);
    const UINT snapshots = GetDlgItemInt(dialog, IDC_SNAPSHOTS, &snapshotsValid, FALSE);
    const bool enabled = IsDlgButtonChecked(dialog, IDC_AUTOSAVE_ENABLED) == BST_CHECKED;
    const bool greatBattles = IsDlgButtonChecked(dialog, IDC_GREAT_BATTLES) == BST_CHECKED;
    const bool korea = IsDlgButtonChecked(dialog, IDC_KOREA) == BST_CHECKED;
    if (enabled && !greatBattles && !korea) {
        MessageBoxW(dialog, L"Select Great Battles, Korea, or both before enabling autosave.",
                    L"IL-2 Mission Guard settings", MB_OK | MB_ICONWARNING);
        return TRUE;
    }
    if (!intervalValid || interval < 1 || interval > 60 ||
        !snapshotsValid || snapshots < 1 || snapshots > 100) {
        MessageBoxW(dialog, L"Save interval must be 1-60 minutes and recovery points must be 1-100.",
                    L"IL-2 Mission Guard settings", MB_OK | MB_ICONWARNING);
        return TRUE;
    }

    state->options = {
        enabled,
        greatBattles,
        korea,
        static_cast<int>(interval),
        static_cast<int>(snapshots),
        IsDlgButtonChecked(dialog, IDC_NOTIFICATIONS) == BST_CHECKED};
    try {
        il2mec::SaveAutoSaveOptions(state->settingsPath, state->options);
        state->saved = true;
        EndDialog(dialog, IDOK);
    } catch (const std::exception& error) {
        MessageBoxW(dialog, (L"The settings could not be saved.\n\n" + il2mec::Utf8ToWide(error.what())).c_str(),
                    L"IL-2 Mission Guard settings", MB_OK | MB_ICONERROR);
    }
    return TRUE;
}

std::wstring GetWindowTextString(HWND window) {
    int length = GetWindowTextLengthW(window);
    std::wstring result(static_cast<std::size_t>(length + 1), L'\0');
    int copied = GetWindowTextW(window, result.data(), length + 1);
    result.resize(static_cast<std::size_t>(std::max(copied, 0)));
    return result;
}

HWND FindMainWindow(DWORD processId) {
    struct State { DWORD id; HWND window; } state{processId, nullptr};
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto* state = reinterpret_cast<State*>(parameter);
        DWORD id = 0; GetWindowThreadProcessId(window, &id);
        if (id == state->id && GetWindow(window, GW_OWNER) == nullptr && IsWindowVisible(window)) {
            state->window = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&state));
    return state.window;
}

std::vector<ProcessInfo> FindProcesses(const std::set<std::wstring>& wantedNames) {
    std::vector<ProcessInfo> result;
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W entry{sizeof(entry)};
    if (!Process32FirstW(snapshot.get(), &entry)) return result;
    do {
        fs::path image(entry.szExeFile);
        std::wstring stem = image.stem().wstring();
        if (!wantedNames.contains(stem)) continue;
        HWND window = FindMainWindow(entry.th32ProcessID);
        if (!window) continue;
        fs::path executable;
        UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID));
        if (process) {
            std::wstring buffer(32768, L'\0');
            DWORD size = static_cast<DWORD>(buffer.size());
            if (QueryFullProcessImageNameW(process.get(), 0, buffer.data(), &size)) {
                buffer.resize(size); executable = buffer;
            }
        }
        result.push_back({entry.th32ProcessID, stem, executable, window, GetWindowTextString(window)});
    } while (Process32NextW(snapshot.get(), &entry));
    return result;
}

bool SamePath(const fs::path& a, const fs::path& b) {
    return CompareStringOrdinal(fs::absolute(a).lexically_normal().c_str(), -1,
                                fs::absolute(b).lexically_normal().c_str(), -1, TRUE) == CSTR_EQUAL;
}

fs::path CanonicalMission(fs::path path) {
    path = fs::absolute(path).lexically_normal();
    if (_wcsicmp(path.extension().c_str(), L".msnbin") == 0) path.replace_extension(L".Mission");
    if (_wcsicmp(path.extension().c_str(), L".Mission") != 0) throw std::runtime_error("Not a mission path.");
    return path;
}

bool SameMission(const fs::path& a, const fs::path& b) {
    try { return SamePath(CanonicalMission(a), CanonicalMission(b)); } catch (...) { return false; }
}

struct RegistryValueBackup {
    std::wstring name;
    bool existed = false;
    DWORD type = REG_NONE;
    std::vector<BYTE> data;

    static RegistryValueBackup Capture(HKEY key, std::wstring name) {
        RegistryValueBackup value; value.name = std::move(name);
        DWORD size = 0;
        LONG result = RegQueryValueExW(key, value.name.c_str(), nullptr, &value.type, nullptr, &size);
        if (result == ERROR_FILE_NOT_FOUND) return value;
        if (result != ERROR_SUCCESS) throw std::runtime_error("Could not read Mission Editor startup settings.");
        value.existed = true; value.data.resize(size);
        if (size && RegQueryValueExW(key, value.name.c_str(), nullptr, &value.type, value.data.data(), &size) != ERROR_SUCCESS)
            throw std::runtime_error("Could not read Mission Editor startup settings.");
        return value;
    }

    void Restore(HKEY key) const {
        if (!existed) RegDeleteValueW(key, name.c_str());
        else if (RegSetValueExW(key, name.c_str(), 0, type, data.empty() ? nullptr : data.data(), static_cast<DWORD>(data.size())) != ERROR_SUCCESS)
            throw std::runtime_error("Could not restore Mission Editor startup settings.");
    }
};

std::optional<std::wstring> QueryRegistryString(HKEY key, const wchar_t* name) {
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return std::nullopt;
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &size) != ERROR_SUCCESS) return std::nullopt;
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

std::optional<std::wstring> FindOptionsKey(const std::wstring& processName, const fs::path& missionPath) {
    if (_wcsicmp(processName.c_str(), L"STEditor") == 0) return L"Software\\1CGS_IL2\\STEditor\\EditorOptions";
    if (_wcsicmp(processName.c_str(), L"IL2Editor") != 0) return std::nullopt;
    HKEY rawRoot = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\1CGS", 0, KEY_READ, &rawRoot) != ERROR_SUCCESS) return std::nullopt;
    std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)> root(rawRoot, RegCloseKey);
    std::vector<std::wstring> candidates;
    for (DWORD index = 0;; ++index) {
        wchar_t name[256]{}; DWORD length = static_cast<DWORD>(std::size(name));
        LONG result = RegEnumKeyExW(root.get(), index, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS) break;
        if (result != ERROR_SUCCESS || _wcsnicmp(name, L"STEditor_", 9) != 0) continue;
        std::wstring relative = L"Software\\1CGS\\" + std::wstring(name, length) + L"\\EditorOptions";
        HKEY rawOptions = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, relative.c_str(), 0, KEY_READ, &rawOptions) != ERROR_SUCCESS) continue;
        std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)> options(rawOptions, RegCloseKey);
        candidates.push_back(relative);
        if (auto last = QueryRegistryString(options.get(), L"LastUsedMissionName"); last && SameMission(*last, missionPath)) return relative;
    }
    if (candidates.size() == 1) return candidates.front();
    return std::nullopt;
}

bool OpenMission(const fs::path& executable, const std::wstring& processName, const fs::path& missionPath,
                 std::chrono::seconds timeout, std::wstring& diagnostic) {
    if (!fs::is_regular_file(executable) || !fs::is_regular_file(missionPath)) { diagnostic = L"The editor executable or restored mission was unavailable."; return false; }
    auto keyPath = FindOptionsKey(processName, missionPath);
    if (!keyPath) { diagnostic = L"The Mission Editor registry profile could not be identified safely."; return false; }
    HKEY rawKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPath->c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &rawKey, nullptr) != ERROR_SUCCESS) {
        diagnostic = L"The Mission Editor startup settings could not be opened."; return false;
    }
    std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)> key(rawKey, RegCloseKey);
    std::vector<RegistryValueBackup> backups{
        RegistryValueBackup::Capture(key.get(), L"OpenLastUsedMissionOnStart"),
        RegistryValueBackup::Capture(key.get(), L"LastUsedMissionName"),
        RegistryValueBackup::Capture(key.get(), L"LastUsedMissionFolder")};
    auto restore = [&] { for (const auto& backup : backups) backup.Restore(key.get()); RegFlushKey(key.get()); };
    try {
        fs::path startupMission = missionPath;
        fs::path binary = missionPath; binary.replace_extension(L".msnbin");
        if (_wcsicmp(processName.c_str(), L"IL2Editor") == 0 && fs::is_regular_file(binary)) startupMission = binary;
        DWORD enabled = 1;
        std::wstring mission = startupMission.wstring();
        std::wstring folder = missionPath.parent_path().wstring(); if (!folder.ends_with(L'\\')) folder += L'\\';
        if (RegSetValueExW(key.get(), L"OpenLastUsedMissionOnStart", 0, REG_DWORD, reinterpret_cast<BYTE*>(&enabled), sizeof(enabled)) != ERROR_SUCCESS ||
            RegSetValueExW(key.get(), L"LastUsedMissionName", 0, REG_SZ, reinterpret_cast<const BYTE*>(mission.c_str()), static_cast<DWORD>((mission.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS ||
            RegSetValueExW(key.get(), L"LastUsedMissionFolder", 0, REG_SZ, reinterpret_cast<const BYTE*>(folder.c_str()), static_cast<DWORD>((folder.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS)
            throw std::runtime_error("Could not set Mission Editor startup settings.");
        RegFlushKey(key.get());
        std::wstring command = L"\"" + executable.wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, executable.parent_path().c_str(), &startup, &process))
            throw std::runtime_error("The Mission Editor process could not be started: " + il2mec::WideToUtf8(il2mec::Win32ErrorMessage()));
        CloseHandle(process.hThread); UniqueHandle processHandle(process.hProcess);
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (WaitForSingleObject(processHandle.get(), 0) == WAIT_OBJECT_0) { diagnostic = L"The Mission Editor exited before the restored mission finished opening."; restore(); return false; }
            HWND window = FindMainWindow(process.dwProcessId);
            fs::path opened;
            if (window && il2mec::TryGetSavedMissionPath(GetWindowTextString(window), opened, false) && SamePath(opened, missionPath)) { restore(); return true; }
            std::this_thread::sleep_for(100ms);
        }
        diagnostic = L"Timed out waiting for the restored mission to open.";
        restore(); return false;
    } catch (...) {
        try { restore(); } catch (...) {}
        throw;
    }
}

class TrayApp {
public:
    TrayApp(HINSTANCE instance, fs::path settingsPath, bool openSettingsOnStart)
        : instance_(instance), settingsPath_(std::move(settingsPath)), options_(il2mec::LoadAutoSaveOptions(settingsPath_)),
          store_(il2mec::DefaultSnapshotRoot(), options_.historicSnapshots) {
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = instance_;
        windowClass.lpszClassName = kWindowClass;
        windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
        RegisterClassExW(&windowClass);
        window_ = CreateWindowExW(0, kWindowClass, L"IL-2 Mission Guard", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, instance_, this);
        if (!window_) throw std::runtime_error("Could not create the autosave notification window.");
        stopEvent_.reset(OpenEventW(SYNCHRONIZE, FALSE, kStopEventName));
        if (!stopEvent_) stopEvent_.reset(CreateEventW(nullptr, FALSE, FALSE, kStopEventName));
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        try {
            int imported = store_.ImportLegacySnapshots(il2mec::LegacySnapshotRoot());
            if (imported > 0) Log(L"Imported " + std::to_wstring(imported) + L" legacy autosave files. The legacy copies were retained.");
        } catch (const std::exception& e) { Log(L"Could not import legacy autosave files: " + il2mec::Utf8ToWide(e.what())); }
        AddTrayIcon();
        SetTimer(window_, kTimerId, 2000, nullptr);
        Tick();
        if (openSettingsOnStart && window_) PostMessageW(window_, kOpenSettingsMessage, 0, 0);
    }

    ~TrayApp() {
        if (window_) KillTimer(window_, kTimerId);
        RemoveTrayIcon();
        if (window_) DestroyWindow(window_);
    }

    int Run() {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        return static_cast<int>(message.wParam);
    }

private:
    HINSTANCE instance_{};
    HWND window_{};
    fs::path settingsPath_;
    il2mec::AutoSaveOptions options_;
    il2mec::SnapshotStore store_;
    UniqueHandle stopEvent_;
    UINT taskbarCreated_{};
    NOTIFYICONDATAW tray_{sizeof(tray_)};
    std::map<DWORD, std::chrono::steady_clock::time_point> nextSave_;
    std::vector<il2mec::Snapshot> restoreItems_;

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        TrayApp* app = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = static_cast<TrayApp*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            return TRUE;
        }
        if (!app) return DefWindowProcW(window, message, wParam, lParam);
        try { return app->HandleMessage(message, wParam, lParam); }
        catch (const std::exception& error) {
            app->Log(L"Unexpected tray-agent error: " + il2mec::Utf8ToWide(error.what()));
            return 0;
        }
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == taskbarCreated_) { AddTrayIcon(); return 0; }
        if (message == kOpenSettingsMessage) { ShowSettings(); return 0; }
        switch (message) {
            case WM_TIMER: Tick(); return 0;
            case kTrayCallback:
                if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) ShowMenu();
                return 0;
            case WM_COMMAND: HandleCommand(LOWORD(wParam)); return 0;
            case WM_DESTROY: PostQuitMessage(0); return 0;
            default: return DefWindowProcW(window_, message, wParam, lParam);
        }
    }

    void AddTrayIcon() {
        if (tray_.hIcon) {
            DestroyIcon(tray_.hIcon);
            tray_.hIcon = nullptr;
        }
        tray_ = {sizeof(tray_)};
        tray_.hWnd = window_; tray_.uID = 1; tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        tray_.uCallbackMessage = kTrayCallback;
        tray_.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_TRAY_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
        if (!tray_.hIcon) throw std::runtime_error("The embedded IL-2 Mission Guard tray icon could not be loaded.");
        wcscpy_s(tray_.szTip, L"IL-2 Mission Guard");
        if (!Shell_NotifyIconW(NIM_ADD, &tray_)) {
            DestroyIcon(tray_.hIcon);
            tray_.hIcon = nullptr;
            throw std::runtime_error("Windows could not add the IL-2 Mission Guard tray icon.");
        }
        tray_.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &tray_);
    }

    void RemoveTrayIcon() {
        if (tray_.hWnd) Shell_NotifyIconW(NIM_DELETE, &tray_);
        if (tray_.hIcon) { DestroyIcon(tray_.hIcon); tray_.hIcon = nullptr; }
    }

    void Notify(const std::wstring& title, const std::wstring& message, DWORD flags) {
        if (!options_.trayNotifications) return;
        tray_.uFlags = NIF_INFO;
        wcsncpy_s(tray_.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(tray_.szInfo, message.c_str(), _TRUNCATE);
        tray_.dwInfoFlags = flags;
        Shell_NotifyIconW(NIM_MODIFY, &tray_);
    }

    void Log(const std::wstring& message) const {
        try {
            fs::create_directories(il2mec::DefaultLogPath().parent_path());
            SYSTEMTIME time{}; GetLocalTime(&time);
            char timestamp[64]{};
            sprintf_s(timestamp, "%04u-%02u-%02uT%02u:%02u:%02u.%03u ", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
            std::ofstream output(il2mec::DefaultLogPath(), std::ios::binary | std::ios::app);
            output << timestamp << il2mec::WideToUtf8(message) << "\r\n";
        } catch (...) {}
    }

    std::set<std::wstring> EnabledNames() const {
        std::set<std::wstring> names;
        if (options_.greatBattles) names.insert(L"STEditor");
        if (options_.korea) names.insert(L"IL2Editor");
        return names;
    }

    void Tick() {
        if (stopEvent_ && WaitForSingleObject(stopEvent_.get(), 0) == WAIT_OBJECT_0) { DestroyWindow(window_); window_ = nullptr; return; }
        auto loaded = il2mec::LoadAutoSaveOptions(settingsPath_);
        bool scheduleChanged = loaded.enabled != options_.enabled || loaded.intervalMinutes != options_.intervalMinutes ||
                               loaded.greatBattles != options_.greatBattles || loaded.korea != options_.korea;
        bool retentionChanged = loaded.historicSnapshots != options_.historicSnapshots;
        options_ = loaded;
        if (scheduleChanged) nextSave_.clear();
        if (retentionChanged) { store_ = il2mec::SnapshotStore(il2mec::DefaultSnapshotRoot(), options_.historicSnapshots); store_.PruneToRetentionLimit(); }
        tray_.uFlags = NIF_TIP;
        std::wstring tip = options_.enabled
            ? L"IL-2 Mission Guard: every " + std::to_wstring(options_.intervalMinutes) + L" min"
            : L"IL-2 Mission Guard: autosave disabled";
        wcsncpy_s(tray_.szTip, tip.c_str(), _TRUNCATE); Shell_NotifyIconW(NIM_MODIFY, &tray_);
        if (!options_.enabled) return;
        SaveDue(false);
    }

    void SaveDue(bool force) {
        auto now = std::chrono::steady_clock::now();
        std::set<DWORD> live;
        for (const auto& process : FindProcesses(EnabledNames())) {
            live.insert(process.id);
            auto found = nextSave_.find(process.id);
            if (!force && (found == nextSave_.end() || found->second > now)) {
                if (found == nextSave_.end()) nextSave_[process.id] = now + std::chrono::minutes(options_.intervalMinutes);
                continue;
            }
            nextSave_[process.id] = now + std::chrono::minutes(options_.intervalMinutes);
            TrySave(process);
        }
        std::erase_if(nextSave_, [&](const auto& item) { return !live.contains(item.first); });
    }

    void TrySave(const ProcessInfo& process) {
        if (!IsWindowEnabled(process.window)) { Log(L"Skipped " + process.name + L": its main window is disabled by a modal dialog."); return; }
        fs::path mission;
        if (!il2mec::TryGetSavedMissionPath(process.title, mission)) { Log(L"Skipped " + process.name + L": the mission has not been saved to a named file yet."); return; }
        DWORD_PTR result = 0;
        SetLastError(ERROR_SUCCESS);
        if (!SendMessageTimeoutW(process.window, WM_COMMAND, kSaveCommandId, 0, SMTO_BLOCK | SMTO_ABORTIFHUNG, 10000, &result)) {
            Log(L"Autosave failed for " + mission.wstring() + L": " + il2mec::Win32ErrorMessage()); return;
        }
        if (process.executable.empty()) { Log(L"Autosave saved the mission, but the editor executable path was unavailable."); return; }
        try {
            il2mec::WaitUntilMissionFamilyStable(mission);
            il2mec::Snapshot snapshot;
            for (int attempt = 1;; ++attempt) {
                try { snapshot = store_.CreateSnapshot(mission, process.name, process.executable); break; }
                catch (...) { if (attempt >= 4) throw; std::this_thread::sleep_for(250ms); }
            }
            std::wstring message = L"Recovery point created for " + mission.filename().wstring() + L".";
            Log(message); Notify(L"IL-2 Mission Guard", message, NIIF_INFO);
        } catch (const std::exception& error) {
            Log(L"Autosave saved " + mission.wstring() + L", but its recovery point failed: " + il2mec::Utf8ToWide(error.what()));
            Notify(L"Mission Guard recovery point failed", L"The mission was saved, but the timestamped recovery copy failed. See the autosave log.", NIIF_WARNING);
        }
    }

    std::vector<fs::path> ActiveMissionPaths() const {
        std::vector<fs::path> result;
        for (const auto& process : FindProcesses(EnabledNames())) {
            fs::path mission;
            if (il2mec::TryGetSavedMissionPath(process.title, mission)) result.push_back(mission);
        }
        return result;
    }

    void ShowMenu() {
        restoreItems_.clear();
        auto active = ActiveMissionPaths();
        if (active.empty()) restoreItems_ = store_.ListSnapshots(std::nullopt, options_.historicSnapshots);
        else {
            for (const auto& path : active) {
                auto items = store_.ListSnapshots(path, options_.historicSnapshots);
                restoreItems_.insert(restoreItems_.end(), items.begin(), items.end());
            }
            std::sort(restoreItems_.begin(), restoreItems_.end(), [](const auto& a, const auto& b) { return a.createdUtc > b.createdUtc; });
            if (restoreItems_.size() > static_cast<std::size_t>(options_.historicSnapshots)) restoreItems_.resize(options_.historicSnapshots);
        }
        HMENU menu = CreatePopupMenu(), restore = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kMenuSave, L"Create recovery point now");
        if (restoreItems_.empty()) AppendMenuW(restore, MF_STRING | MF_GRAYED, 0, L"No recovery points available");
        for (std::size_t i = 0; i < restoreItems_.size() && kRestoreFirst + i <= kRestoreLast; ++i) {
            const auto& snapshot = restoreItems_[i];
            std::wstring text = (snapshot.IsRestorable() ? L"" : L"[DAMAGED]  ") + il2mec::FormatLocalMenuTime(snapshot.createdUtc) + L"  \u2014  " + snapshot.missionPath.stem().wstring();
            AppendMenuW(restore, MF_STRING | (snapshot.IsRestorable() ? 0 : MF_GRAYED), kRestoreFirst + static_cast<UINT>(i), text.c_str());
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(restore), L"Restore previous autosave");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuSettings, L"Settings...");
        AppendMenuW(menu, MF_STRING, kMenuFolder, L"Open autosave folder");
        AppendMenuW(menu, MF_STRING, kMenuLog, L"Open autosave log");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");
        POINT point{}; GetCursorPos(&point); SetForegroundWindow(window_);
        UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, point.x, point.y, 0, window_, nullptr);
        DestroyMenu(menu);
        if (command) HandleCommand(command);
    }

    void HandleCommand(UINT command) {
        if (command == kMenuSave) SaveDue(true);
        else if (command == kMenuSettings) ShowSettings();
        else if (command == kMenuFolder) OpenPath(store_.RootDirectory(), true);
        else if (command == kMenuLog) { Log(L"Autosave log opened from the tray menu."); OpenPath(il2mec::DefaultLogPath(), false); }
        else if (command == kMenuExit) { DestroyWindow(window_); window_ = nullptr; }
        else if (command >= kRestoreFirst && command <= kRestoreLast && command - kRestoreFirst < restoreItems_.size()) Restore(restoreItems_[command - kRestoreFirst]);
    }

    void ShowSettings() {
        SettingsDialogState state{options_, settingsPath_, instance_, false};
        const INT_PTR result = DialogBoxParamW(
            instance_, MAKEINTRESOURCEW(IDD_SETTINGS), nullptr,
            SettingsDialogProcedure, reinterpret_cast<LPARAM>(&state));
        if (result == -1) {
            const std::wstring error = L"Windows could not open the settings dialog.\n\n" + il2mec::Win32ErrorMessage();
            Log(error);
            MessageBoxW(nullptr, error.c_str(), L"IL-2 Mission Guard", MB_OK | MB_ICONERROR);
            return;
        }
        if (state.saved) {
            Log(L"Autosave settings were updated from the tray menu.");
            Tick();
        }
    }

    void OpenPath(const fs::path& path, bool directory) {
        try {
            if (directory) fs::create_directories(path);
            else if (!fs::exists(path)) { fs::create_directories(path.parent_path()); std::ofstream(path).close(); }
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                throw std::runtime_error("Windows could not open the requested path.");
        } catch (const std::exception& error) {
            Log(L"Could not open path: " + il2mec::Utf8ToWide(error.what()));
            MessageBoxW(nullptr, il2mec::Utf8ToWide(error.what()).c_str(), L"IL-2 Mission Guard", MB_OK | MB_ICONERROR);
        }
    }

    std::optional<ProcessInfo> FindOpenEditor(const il2mec::Snapshot& snapshot) const {
        for (const auto& process : FindProcesses({snapshot.editorProcessName})) {
            fs::path mission;
            if (il2mec::TryGetSavedMissionPath(process.title, mission) && SamePath(mission, snapshot.missionPath)) return process;
        }
        return std::nullopt;
    }

    void Restore(const il2mec::Snapshot& snapshot) {
        std::wstring prompt = L"Restore " + snapshot.missionPath.filename().wstring() + L" to the recovery point from " +
            il2mec::FormatLocalMenuTime(snapshot.createdUtc) + L"?\n\nIf this mission is open, IL-2 Mission Guard will ask the Mission Editor to close. "
            L"If the editor asks whether to save your current changes, choose Don't Save. Restoration will not begin until the editor has closed.\n\n"
            L"A separate safety copy of the current on-disk files will be made before anything is replaced.";
        if (MessageBoxW(nullptr, prompt.c_str(), L"Restore IL-2 mission recovery point", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
        try {
            if (auto editor = FindOpenEditor(snapshot)) {
                UniqueHandle process(OpenProcess(SYNCHRONIZE, FALSE, editor->id));
                if (!PostMessageW(editor->window, WM_CLOSE, 0, 0)) throw std::runtime_error("The Mission Editor did not accept the close request. Close it manually and try again.");
                if (!process || WaitForSingleObject(process.get(), 120000) != WAIT_OBJECT_0) throw std::runtime_error("The Mission Editor did not close within two minutes. No files were restored.");
            }
            auto restored = store_.RestoreSnapshot(snapshot);
            std::wstring diagnostic;
            bool reopened = false;
            try { reopened = OpenMission(snapshot.editorExecutablePath, snapshot.editorProcessName, restored.missionPath, 120s, diagnostic); }
            catch (const std::exception& error) { diagnostic = il2mec::Utf8ToWide(error.what()); }
            if (reopened) {
                std::wstring message = L"Restored " + snapshot.missionPath.filename().wstring() + L" and reopened it in the Mission Editor.";
                Log(message + L" Safety backup: " + restored.safetyBackupDirectory.wstring());
                Notify(L"Mission Guard restored the mission", message, NIIF_INFO);
            } else {
                std::wstring message = L"Restored " + snapshot.missionPath.filename().wstring() + L", but it could not be reopened automatically. Open the mission manually.";
                Log(message + L" " + diagnostic + L" Safety backup: " + restored.safetyBackupDirectory.wstring());
                Notify(L"Mission Guard could not reopen the mission", message, NIIF_WARNING);
                MessageBoxW(nullptr, (message + L"\n\n" + diagnostic + L"\n\nSafety copy of the files replaced:\n" + restored.safetyBackupDirectory.wstring()).c_str(),
                            L"Mission reopen failed", MB_OK | MB_ICONERROR);
            }
        } catch (const std::exception& error) {
            std::wstring message = L"The mission was not restored.\n\n" + il2mec::Utf8ToWide(error.what());
            Log(L"Mission restore failed for " + snapshot.missionPath.wstring() + L": " + il2mec::Utf8ToWide(error.what()));
            MessageBoxW(nullptr, message.c_str(), L"Mission recovery failed", MB_OK | MB_ICONERROR);
        }
    }
};

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    try {
        const bool openSettings = commandLine && wcsstr(commandLine, L"--settings") != nullptr;
        UniqueHandle mutex(CreateMutexW(nullptr, TRUE, kMutexName));
        if (!mutex) return 0;
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (openSettings) {
                HWND existing = FindWindowExW(HWND_MESSAGE, nullptr, kWindowClass, nullptr);
                if (existing) PostMessageW(existing, kOpenSettingsMessage, 0, 0);
            }
            return 0;
        }
        wchar_t settingsOverride[32768]{};
        DWORD length = GetEnvironmentVariableW(L"IL2MISSIONGUARD_SETTINGS_FILE", settingsOverride, static_cast<DWORD>(std::size(settingsOverride)));
        if (length == 0) {
            length = GetEnvironmentVariableW(L"IL2MEC_SETTINGS_FILE", settingsOverride, static_cast<DWORD>(std::size(settingsOverride)));
        }
        fs::path settings = length > 0 && length < std::size(settingsOverride) ? fs::path(settingsOverride) : il2mec::DefaultSettingsPath();
        TrayApp app(instance, settings, openSettings);
        return app.Run();
    } catch (const std::exception& error) {
        MessageBoxW(nullptr, il2mec::Utf8ToWide(error.what()).c_str(), L"IL-2 Mission Guard", MB_OK | MB_ICONERROR);
        return 1;
    }
}
