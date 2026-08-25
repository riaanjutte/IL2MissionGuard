#include "NativeCore.h"

#include <bcrypt.h>
#include <shlobj.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;

namespace missionguard {
namespace {

constexpr wchar_t kMetadataSuffix[] = L".missionguard-autosave.json";

std::wstring Trim(std::wstring value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring Lower(std::wstring value) {
    if (!value.empty()) CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}

bool EqualsInsensitive(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
                                static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool StartsWithInsensitive(const std::wstring& value, const std::wstring& prefix) {
    return value.size() >= prefix.size() &&
           CompareStringOrdinal(value.c_str(), static_cast<int>(prefix.size()), prefix.c_str(),
                                static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

std::wstring NewGuidText() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Could not create an operation identifier.");
    wchar_t buffer[40]{};
    StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
    std::wstring value(buffer);
    value.erase(std::remove_if(value.begin(), value.end(), [](wchar_t c) { return c == L'{' || c == L'}' || c == L'-'; }), value.end());
    return value;
}

fs::path FullPath(const fs::path& value) {
    return fs::absolute(value).lexically_normal();
}

bool IsMissionExtension(const fs::path& path) {
    return EqualsInsensitive(path.extension().wstring(), L".Mission");
}

fs::path NormalizeMissionPath(const fs::path& value, bool requireExists) {
    if (value.empty() || !value.is_absolute()) throw std::invalid_argument("The mission path must be fully qualified.");
    fs::path path = FullPath(value);
    if (!IsMissionExtension(path)) throw std::invalid_argument("The autosave source must be an IL-2 .Mission file.");
    if (requireExists && !fs::is_regular_file(path)) throw std::runtime_error("The mission file does not exist: " + WideToUtf8(path.wstring()));
    return path;
}

std::wstring SanitizeFileName(std::wstring value) {
    constexpr wchar_t invalid[] = L"<>:\"/\\|?*";
    for (auto& c : value) {
        if (c < 32 || std::wcschr(invalid, c) != nullptr) c = L'_';
    }
    value = Trim(value);
    while (!value.empty() && value.back() == L'.') value.pop_back();
    if (value.empty()) value = L"Mission";
    if (value.size() > 80) value.resize(80);
    return value;
}

std::wstring HashBytes(const unsigned char* bytes, std::size_t size) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, resultSize = 0, hashSize = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &resultSize, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize), &resultSize, 0) < 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("Windows could not initialize SHA-256.");
    }
    std::vector<unsigned char> object(objectSize), digest(hashSize);
    NTSTATUS status = BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0);
    if (status >= 0) status = BCryptHashData(hash, const_cast<PUCHAR>(bytes), static_cast<ULONG>(size), 0);
    if (status >= 0) status = BCryptFinishHash(hash, digest.data(), hashSize, 0);
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("Windows could not calculate SHA-256.");
    static constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring result;
    result.reserve(digest.size() * 2);
    for (unsigned char byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}

std::wstring BuildMissionKey(const fs::path& missionPath) {
    std::wstring normalized = FullPath(missionPath).wstring();
    if (!normalized.empty()) CharUpperBuffW(normalized.data(), static_cast<DWORD>(normalized.size()));
    std::string utf8 = WideToUtf8(normalized);
    std::wstring hash = HashBytes(reinterpret_cast<const unsigned char*>(utf8.data()), utf8.size());
    return SanitizeFileName(missionPath.stem().wstring()) + L"_" + hash.substr(0, 12);
}

void CopyFileAtomically(const fs::path& source, const fs::path& destination, bool replace = false) {
    fs::path temporary = destination.wstring() + L".tmp-" + NewGuidText();
    try {
        fs::copy_file(source, temporary, fs::copy_options::none);
        if (replace) {
            if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Could not replace " + WideToUtf8(destination.wstring()) + ": " + WideToUtf8(Win32ErrorMessage()));
        } else {
            fs::rename(temporary, destination);
        }
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

std::string ReadUtf8File(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not read " + WideToUtf8(path.wstring()));
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void WriteUtf8Atomically(const fs::path& path, const std::string& contents) {
    fs::path temporary = path.wstring() + L".tmp-" + NewGuidText();
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Could not write autosave metadata.");
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.close();
        fs::rename(temporary, path);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

std::string EscapeJson(const std::wstring& value) {
    std::string utf8 = WideToUtf8(value), result;
    result.reserve(utf8.size() + 8);
    for (unsigned char c : utf8) {
        switch (c) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[7]{};
                    sprintf_s(buffer, "\\u%04X", c);
                    result += buffer;
                } else result.push_back(static_cast<char>(c));
        }
    }
    return result;
}

std::string FormatUtcIso(std::chrono::system_clock::time_point value) {
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(value);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(value - seconds).count();
    const std::time_t time = std::chrono::system_clock::to_time_t(seconds);
    std::tm tm{};
    gmtime_s(&tm, &time);
    std::ostringstream output;
    output << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << milliseconds << "0000+00:00";
    return output.str();
}

std::chrono::system_clock::time_point ParseIsoTime(const std::string& text) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (sscanf_s(text.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6)
        throw std::runtime_error("Autosave metadata contains an invalid timestamp.");
    std::tm tm{};
    tm.tm_year = year - 1900; tm.tm_mon = month - 1; tm.tm_mday = day;
    tm.tm_hour = hour; tm.tm_min = minute; tm.tm_sec = second;
    const std::time_t utc = _mkgmtime(&tm);
    if (utc == -1) throw std::runtime_error("Autosave metadata contains an invalid timestamp.");
    std::chrono::milliseconds fractional{0};
    const auto dot = text.find('.', 19);
    std::size_t zone = text.find_first_of("Z+-", 19);
    if (dot != std::string::npos && (zone == std::string::npos || dot < zone)) {
        std::string digits = text.substr(dot + 1, (zone == std::string::npos ? text.size() : zone) - dot - 1);
        while (digits.size() < 3) digits.push_back('0');
        digits.resize(3);
        fractional = std::chrono::milliseconds(std::stoi(digits));
    }
    auto result = std::chrono::system_clock::from_time_t(utc) + fractional;
    if (zone != std::string::npos && text[zone] != 'Z') {
        int zh = 0, zm = 0;
        if (sscanf_s(text.c_str() + zone + 1, "%d:%d", &zh, &zm) == 2) {
            auto offset = std::chrono::minutes(zh * 60 + zm);
            result += text[zone] == '+' ? -offset : offset;
        }
    }
    return result;
}

class JsonParser {
public:
    explicit JsonParser(const std::string& source) : source_(source) {}

    struct Value {
        enum class Type { Null, String, Number, Object, Array, Boolean } type = Type::Null;
        std::string string;
        long long number = 0;
        bool boolean = false;
        std::map<std::string, Value> object;
        std::vector<Value> array;
    };

    Value Parse() {
        Value value = ParseValue();
        Skip();
        if (position_ != source_.size()) throw std::runtime_error("Unexpected text after autosave metadata.");
        return value;
    }

private:
    const std::string& source_;
    std::size_t position_ = 0;

    void Skip() { while (position_ < source_.size() && std::isspace(static_cast<unsigned char>(source_[position_]))) ++position_; }
    char Take() { if (position_ >= source_.size()) throw std::runtime_error("Unexpected end of autosave metadata."); return source_[position_++]; }
    void Expect(char c) { Skip(); if (Take() != c) throw std::runtime_error("Invalid autosave metadata JSON."); }

    Value ParseValue() {
        Skip();
        if (position_ >= source_.size()) throw std::runtime_error("Unexpected end of autosave metadata.");
        const char c = source_[position_];
        if (c == '{') return ParseObject();
        if (c == '[') return ParseArray();
        if (c == '"') { Value v; v.type = Value::Type::String; v.string = ParseString(); return v; }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return ParseNumber();
        if (source_.compare(position_, 4, "true") == 0) { position_ += 4; Value v; v.type = Value::Type::Boolean; v.boolean = true; return v; }
        if (source_.compare(position_, 5, "false") == 0) { position_ += 5; Value v; v.type = Value::Type::Boolean; return v; }
        if (source_.compare(position_, 4, "null") == 0) { position_ += 4; return {}; }
        throw std::runtime_error("Invalid autosave metadata JSON value.");
    }

    Value ParseObject() {
        Value v; v.type = Value::Type::Object; Expect('{'); Skip();
        if (position_ < source_.size() && source_[position_] == '}') { ++position_; return v; }
        for (;;) {
            Skip();
            std::string key = ParseString();
            Expect(':');
            v.object.emplace(std::move(key), ParseValue());
            Skip();
            char c = Take();
            if (c == '}') return v;
            if (c != ',') throw std::runtime_error("Invalid autosave metadata object.");
        }
    }

    Value ParseArray() {
        Value v; v.type = Value::Type::Array; Expect('['); Skip();
        if (position_ < source_.size() && source_[position_] == ']') { ++position_; return v; }
        for (;;) {
            v.array.push_back(ParseValue());
            Skip();
            char c = Take();
            if (c == ']') return v;
            if (c != ',') throw std::runtime_error("Invalid autosave metadata array.");
        }
    }

    std::string ParseString() {
        Skip();
        if (Take() != '"') throw std::runtime_error("Invalid autosave metadata string.");
        std::string result;
        while (position_ < source_.size()) {
            char c = Take();
            if (c == '"') return result;
            if (c != '\\') { result.push_back(c); continue; }
            char escaped = Take();
            switch (escaped) {
                case '"': result.push_back('"'); break; case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break; case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break; case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break; case 't': result.push_back('\t'); break;
                case 'u': {
                    if (position_ + 4 > source_.size()) throw std::runtime_error("Invalid JSON escape.");
                    unsigned int code = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = Take(); code <<= 4;
                        if (h >= '0' && h <= '9') code += h - '0';
                        else if (h >= 'A' && h <= 'F') code += h - 'A' + 10;
                        else if (h >= 'a' && h <= 'f') code += h - 'a' + 10;
                        else throw std::runtime_error("Invalid JSON escape.");
                    }
                    wchar_t wide[2] = {static_cast<wchar_t>(code), 0};
                    result += WideToUtf8(wide);
                    break;
                }
                default: throw std::runtime_error("Invalid JSON escape.");
            }
        }
        throw std::runtime_error("Unterminated autosave metadata string.");
    }

    Value ParseNumber() {
        Skip();
        std::size_t start = position_;
        if (source_[position_] == '-') ++position_;
        while (position_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[position_]))) ++position_;
        Value v; v.type = Value::Type::Number;
        auto [ptr, error] = std::from_chars(source_.data() + start, source_.data() + position_, v.number);
        if (error != std::errc()) throw std::runtime_error("Invalid autosave metadata number.");
        return v;
    }
};

const JsonParser::Value& Member(const JsonParser::Value& object, const char* name, JsonParser::Value::Type type) {
    auto found = object.object.find(name);
    if (found == object.object.end() || found->second.type != type) throw std::runtime_error(std::string("Missing autosave metadata member: ") + name);
    return found->second;
}

bool HasPrefix(const std::wstring& value, const std::wstring& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool IsSha256(const std::wstring& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
    });
}

std::string SerializeSnapshot(const Snapshot& snapshot) {
    std::ostringstream output;
    output << "{\n  \"SchemaVersion\": 1,\n"
           << "  \"MissionPath\": \"" << EscapeJson(snapshot.missionPath.wstring()) << "\",\n"
           << "  \"EditorProcessName\": \"" << EscapeJson(snapshot.editorProcessName) << "\",\n"
           << "  \"EditorExecutablePath\": \"" << EscapeJson(snapshot.editorExecutablePath.wstring()) << "\",\n"
           << "  \"CreatedUtc\": \"" << FormatUtcIso(snapshot.createdUtc) << "\",\n"
           << "  \"Files\": [\n";
    for (std::size_t i = 0; i < snapshot.files.size(); ++i) {
        const auto& file = snapshot.files[i];
        output << "    {\n"
               << "      \"OriginalFileName\": \"" << EscapeJson(file.originalFileName) << "\",\n"
               << "      \"SnapshotFileName\": \"" << EscapeJson(file.snapshotFileName) << "\",\n"
               << "      \"Length\": " << file.length << ",\n"
               << "      \"Sha256\": \"" << EscapeJson(file.sha256) << "\"\n"
               << "    }" << (i + 1 == snapshot.files.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    return output.str();
}

fs::path ResolveChildPath(const fs::path& directory, const std::wstring& fileName) {
    if (fileName.empty() || fs::path(fileName).filename().wstring() != fileName)
        throw std::runtime_error("Autosave metadata contains an invalid stored filename.");
    fs::path root = FullPath(directory);
    fs::path path = FullPath(root / fileName);
    if (!StartsWithInsensitive(path.wstring(), (root.wstring() + L"\\")))
        throw std::runtime_error("Autosave metadata points outside its snapshot directory.");
    return path;
}

fs::path ResolveMissionCompanionPath(const fs::path& missionPath, const std::wstring& fileName) {
    if (fileName.empty() || fs::path(fileName).filename().wstring() != fileName ||
        !EqualsInsensitive(fs::path(fileName).stem().wstring(), missionPath.stem().wstring()))
        throw std::runtime_error("Autosave metadata contains an invalid mission companion filename.");
    return missionPath.parent_path() / fileName;
}

std::vector<std::tuple<fs::path, std::uintmax_t, fs::file_time_type>> CaptureFamily(const fs::path& missionPath) {
    auto files = EnumerateMissionFamily(missionPath);
    if (std::none_of(files.begin(), files.end(), [&](const fs::path& path) { return EqualsInsensitive(path.wstring(), missionPath.wstring()); }))
        throw std::runtime_error("The saved .Mission file disappeared before a recovery point could be created.");
    std::vector<std::tuple<fs::path, std::uintmax_t, fs::file_time_type>> states;
    for (const auto& path : files) states.emplace_back(path, fs::file_size(path), fs::last_write_time(path));
    return states;
}

std::wstring FormatLocal(std::chrono::system_clock::time_point value, const wchar_t* format) {
    const std::time_t time = std::chrono::system_clock::to_time_t(value);
    std::tm tm{}; localtime_s(&tm, &time);
    wchar_t buffer[128]{};
    wcsftime(buffer, std::size(buffer), format, &tm);
    return buffer;
}

}  // namespace

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("Could not encode Unicode text.");
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("Could not decode Unicode text.");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring Win32ErrorMessage(DWORD error) {
    wchar_t* text = nullptr;
    DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                  nullptr, error, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring result = length && text ? Trim(std::wstring(text, length)) : L"Windows error " + std::to_wstring(error);
    if (text) LocalFree(text);
    return result;
}

fs::path LocalAppDataDirectory() {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &value)))
        throw std::runtime_error("Could not locate Local AppData.");
    fs::path result(value); CoTaskMemFree(value); return result / L"IL2MissionGuard";
}

fs::path DefaultSettingsPath() { return LocalAppDataDirectory() / L"IL2MissionGuard.ini"; }
fs::path DefaultSnapshotRoot() { return LocalAppDataDirectory() / L"Autosave"; }
fs::path DefaultLogPath() { return LocalAppDataDirectory() / L"autosave.log"; }

fs::path LegacySnapshotRoot() {
    wchar_t buffer[MAX_PATH + 1]{};
    DWORD length = GetTempPathW(static_cast<DWORD>(std::size(buffer)), buffer);
    if (!length || length >= std::size(buffer)) throw std::runtime_error("Could not locate the temporary directory.");
    return FullPath(fs::path(buffer) / L"STEditor" / L"Autosave");
}

AutoSaveOptions LoadAutoSaveOptions(const fs::path& settingsPath) {
    AutoSaveOptions options;
    if (!fs::exists(settingsPath)) return options;
    auto read = [&](const wchar_t* key, const wchar_t* fallback) {
        std::array<wchar_t, 64> buffer{};
        GetPrivateProfileStringW(L"AutoSave", key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), settingsPath.c_str());
        return Trim(buffer.data());
    };
    auto boolean = [&](const wchar_t* key, bool fallback) {
        std::wstring value = Lower(read(key, fallback ? L"true" : L"false"));
        return value == L"true" || value == L"1";
    };
    auto integer = [&](const wchar_t* key, int fallback) {
        try { return std::stoi(read(key, std::to_wstring(fallback).c_str())); } catch (...) { return fallback; }
    };
    options.enabled = boolean(L"Enabled", true);
    options.greatBattles = boolean(L"GreatBattles", true);
    options.korea = boolean(L"Korea", true);
    options.intervalMinutes = integer(L"IntervalMinutes", 5);
    options.historicSnapshots = integer(L"HistoricSnapshots", 10);
    options.trayNotifications = boolean(L"TrayNotifications", true);
    const std::wstring theme = Lower(read(L"Theme", L"system"));
    if (theme == L"light") options.theme = ThemeMode::Light;
    else if (theme == L"dark") options.theme = ThemeMode::Dark;
    else options.theme = ThemeMode::System;
    if (options.intervalMinutes < 1 || options.intervalMinutes > 60 || options.historicSnapshots < 1 || options.historicSnapshots > 100 ||
        (options.enabled && !options.greatBattles && !options.korea)) return AutoSaveOptions{};
    return options;
}

void SaveAutoSaveOptions(const fs::path& settingsValue, const AutoSaveOptions& options) {
    if (options.intervalMinutes < 1 || options.intervalMinutes > 60 ||
        options.historicSnapshots < 1 || options.historicSnapshots > 100 ||
        (options.enabled && !options.greatBattles && !options.korea)) {
        throw std::invalid_argument("The autosave settings are outside the supported range.");
    }

    fs::path settingsPath = FullPath(settingsValue);
    fs::create_directories(settingsPath.parent_path());
    const bool existed = fs::is_regular_file(settingsPath);
    const std::string original = existed ? ReadUtf8File(settingsPath) : std::string{};
    auto write = [&](const wchar_t* key, const std::wstring& value) {
        if (!WritePrivateProfileStringW(L"AutoSave", key, value.c_str(), settingsPath.c_str())) {
            throw std::runtime_error("Windows could not update the autosave settings file.");
        }
    };

    try {
        write(L"Enabled", options.enabled ? L"true" : L"false");
        write(L"GreatBattles", options.greatBattles ? L"true" : L"false");
        write(L"Korea", options.korea ? L"true" : L"false");
        write(L"IntervalMinutes", std::to_wstring(options.intervalMinutes));
        write(L"HistoricSnapshots", std::to_wstring(options.historicSnapshots));
        write(L"TrayNotifications", options.trayNotifications ? L"true" : L"false");
        write(L"Theme", options.theme == ThemeMode::Dark ? L"Dark" : options.theme == ThemeMode::Light ? L"Light" : L"System");
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, settingsPath.c_str());
    } catch (...) {
        std::error_code ignored;
        if (existed) WriteUtf8Atomically(settingsPath, original);
        else fs::remove(settingsPath, ignored);
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, settingsPath.c_str());
        throw;
    }
}

std::optional<SemanticVersion> ParseSemanticVersion(const std::wstring& input) {
    std::wstring value = Trim(input);
    if (!value.empty() && (value.front() == L'v' || value.front() == L'V')) value.erase(value.begin());
    const std::size_t prereleaseSeparator = value.find(L'-');
    std::wstring prerelease;
    if (prereleaseSeparator != std::wstring::npos) {
        prerelease = value.substr(prereleaseSeparator + 1);
        value.resize(prereleaseSeparator);
        if (prerelease.empty()) return std::nullopt;
        std::size_t identifierStart = 0;
        while (identifierStart < prerelease.size()) {
            const std::size_t identifierEnd = prerelease.find(L'.', identifierStart);
            const std::wstring identifier = prerelease.substr(
                identifierStart, identifierEnd == std::wstring::npos ? identifierEnd : identifierEnd - identifierStart);
            if (identifier.empty() || !std::all_of(identifier.begin(), identifier.end(), [](wchar_t c) {
                    return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') ||
                           (c >= L'a' && c <= L'z') || c == L'-';
                })) return std::nullopt;
            if (identifier.size() > 1 && identifier.front() == L'0' &&
                std::all_of(identifier.begin(), identifier.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
                return std::nullopt;
            if (identifierEnd == std::wstring::npos) break;
            if (identifierEnd + 1 == prerelease.size()) return std::nullopt;
            identifierStart = identifierEnd + 1;
        }
    }
    SemanticVersion result;
    int* parts[] = {&result.major, &result.minor, &result.patch};
    std::size_t start = 0;
    for (std::size_t index = 0; index < std::size(parts); ++index) {
        const std::size_t end = value.find(L'.', start);
        if ((index < 2 && end == std::wstring::npos) || (index == 2 && end != std::wstring::npos)) return std::nullopt;
        const std::wstring piece = value.substr(start, end == std::wstring::npos ? end : end - start);
        if (piece.empty() || (piece.size() > 1 && piece.front() == L'0') ||
            !std::all_of(piece.begin(), piece.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) return std::nullopt;
        try {
            std::size_t consumed = 0;
            const long parsed = std::stol(piece, &consumed, 10);
            if (consumed != piece.size() || parsed > INT_MAX) return std::nullopt;
            *parts[index] = static_cast<int>(parsed);
        } catch (...) { return std::nullopt; }
        start = end == std::wstring::npos ? value.size() : end + 1;
    }
    result.prerelease = std::move(prerelease);
    return result;
}

bool IsNewerVersion(const std::wstring& candidate, const std::wstring& current) {
    const auto left = ParseSemanticVersion(candidate);
    const auto right = ParseSemanticVersion(current);
    if (!left || !right) throw std::invalid_argument("A release version is not valid semantic version text.");
    if (left->major != right->major) return left->major > right->major;
    if (left->minor != right->minor) return left->minor > right->minor;
    if (left->patch != right->patch) return left->patch > right->patch;
    if (left->prerelease.empty() != right->prerelease.empty()) return left->prerelease.empty();
    if (left->prerelease.empty()) return false;

    std::size_t leftStart = 0, rightStart = 0;
    for (;;) {
        const std::size_t leftEnd = left->prerelease.find(L'.', leftStart);
        const std::size_t rightEnd = right->prerelease.find(L'.', rightStart);
        const std::wstring leftPart = left->prerelease.substr(leftStart, leftEnd == std::wstring::npos ? leftEnd : leftEnd - leftStart);
        const std::wstring rightPart = right->prerelease.substr(rightStart, rightEnd == std::wstring::npos ? rightEnd : rightEnd - rightStart);
        const bool leftNumeric = std::all_of(leftPart.begin(), leftPart.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
        const bool rightNumeric = std::all_of(rightPart.begin(), rightPart.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
        if (leftNumeric != rightNumeric) return !leftNumeric;
        if (leftPart != rightPart) {
            if (leftNumeric) {
                if (leftPart.size() != rightPart.size()) return leftPart.size() > rightPart.size();
            }
            return leftPart > rightPart;
        }
        if (leftEnd == std::wstring::npos || rightEnd == std::wstring::npos)
            return leftEnd != std::wstring::npos;
        leftStart = leftEnd + 1;
        rightStart = rightEnd + 1;
    }
}

GitHubRelease ParseGitHubLatestReleaseJson(const std::string& json) {
    const auto root = JsonParser(json).Parse();
    const JsonParser::Value* releaseJson = &root;
    if (root.type == JsonParser::Value::Type::Array) {
        if (root.array.empty()) throw std::runtime_error("GitHub has no published releases.");
        releaseJson = &root.array.front();
    }
    if (releaseJson->type != JsonParser::Value::Type::Object)
        throw std::runtime_error("The GitHub release response is not an object.");
    GitHubRelease release;
    release.version = Utf8ToWide(Member(*releaseJson, "tag_name", JsonParser::Value::Type::String).string);
    release.releaseUrl = Utf8ToWide(Member(*releaseJson, "html_url", JsonParser::Value::Type::String).string);
    if (!ParseSemanticVersion(release.version)) throw std::runtime_error("The latest GitHub release has an invalid version tag.");

    constexpr wchar_t releasePrefix[] = L"https://github.com/riaanjutte/IL2MissionGuard/releases/";
    constexpr wchar_t assetPrefix[] = L"https://github.com/riaanjutte/IL2MissionGuard/releases/download/";
    if (!HasPrefix(release.releaseUrl, releasePrefix)) throw std::runtime_error("The GitHub release URL is not trusted.");

    const auto& assets = Member(*releaseJson, "assets", JsonParser::Value::Type::Array);
    for (const auto& asset : assets.array) {
        if (asset.type != JsonParser::Value::Type::Object) continue;
        const auto name = asset.object.find("name");
        if (name == asset.object.end() || name->second.type != JsonParser::Value::Type::String || name->second.string != "IL2MissionGuard.exe") continue;
        release.assetUrl = Utf8ToWide(Member(asset, "browser_download_url", JsonParser::Value::Type::String).string);
        std::wstring digest = Utf8ToWide(Member(asset, "digest", JsonParser::Value::Type::String).string);
        if (!HasPrefix(release.assetUrl, assetPrefix)) throw std::runtime_error("The update asset URL is not trusted.");
        if (!HasPrefix(digest, L"sha256:") || !IsSha256(digest.substr(7))) throw std::runtime_error("The update asset has no valid SHA-256 digest.");
        release.sha256 = Lower(digest.substr(7));
        return release;
    }
    throw std::runtime_error("The latest GitHub release does not contain IL2MissionGuard.exe.");
}

bool TryGetSavedMissionPath(const std::wstring& title, fs::path& missionPath, bool requireExists) {
    const auto separator = title.find(L" - ");
    if (separator == std::wstring::npos) return false;
    std::wstring candidate = Trim(title.substr(separator + 3));
    if (candidate.empty() || Lower(candidate).find(L"<empty>") != std::wstring::npos) return false;
    if (!candidate.empty() && candidate.back() == L'*') candidate = Trim(candidate.substr(0, candidate.size() - 1));
    fs::path path(candidate);
    if (!path.is_absolute()) return false;
    if (EqualsInsensitive(path.extension().wstring(), L".msnbin")) path.replace_extension(L".Mission");
    else if (!IsMissionExtension(path)) return false;
    path = FullPath(path);
    if (requireExists && !fs::is_regular_file(path)) return false;
    missionPath = path;
    return true;
}

std::vector<fs::path> EnumerateMissionFamily(const fs::path& value) {
    fs::path missionPath = FullPath(value);
    std::vector<fs::path> result;
    if (!fs::is_directory(missionPath.parent_path())) return result;
    for (const auto& entry : fs::directory_iterator(missionPath.parent_path())) {
        if (entry.is_regular_file() && EqualsInsensitive(entry.path().stem().wstring(), missionPath.stem().wstring()))
            result.push_back(FullPath(entry.path()));
    }
    std::sort(result.begin(), result.end(), [](const fs::path& a, const fs::path& b) { return Lower(a.wstring()) < Lower(b.wstring()); });
    return result;
}

void WaitUntilMissionFamilyStable(const fs::path& value, std::chrono::milliseconds timeout,
                                  std::chrono::milliseconds sampleInterval, int requiredStableObservations,
                                  std::chrono::milliseconds minimumWait) {
    fs::path missionPath = NormalizeMissionPath(value, true);
    if (timeout <= std::chrono::milliseconds::zero() || sampleInterval <= std::chrono::milliseconds::zero() ||
        requiredStableObservations < 2 || minimumWait < std::chrono::milliseconds::zero() || minimumWait > timeout)
        throw std::invalid_argument("Invalid mission stability timing.");
    auto start = std::chrono::steady_clock::now();
    decltype(CaptureFamily(missionPath)) previous;
    int stable = 0;
    while (std::chrono::steady_clock::now() - start <= timeout) {
        auto current = CaptureFamily(missionPath);
        if (!previous.empty() && current == previous) ++stable; else stable = 1;
        if (std::chrono::steady_clock::now() - start >= minimumWait && stable >= requiredStableObservations) return;
        previous = std::move(current);
        std::this_thread::sleep_for(sampleInterval);
    }
    throw std::runtime_error("The Mission Editor did not finish writing the mission files before the recovery-point timeout.");
}

std::wstring Sha256File(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not read a snapshot file for verification.");
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return HashBytes(bytes.data(), bytes.size());
}

std::wstring Sha256Bytes(const void* data, std::size_t size) {
    return HashBytes(static_cast<const unsigned char*>(data), size);
}

std::wstring FormatLocalSnapshotTime(std::chrono::system_clock::time_point value) { return FormatLocal(value, L"%Y-%m-%d_%H-%M-%S"); }
std::wstring FormatLocalMenuTime(std::chrono::system_clock::time_point value) { return FormatLocal(value, L"%d %b %Y  %H:%M:%S"); }

SnapshotStore::SnapshotStore(fs::path root, int retention) : rootDirectory_(FullPath(root)), retentionCount_(retention) {
    if (retention < 1) throw std::invalid_argument("Snapshot retention must be positive.");
}

int SnapshotStore::ImportLegacySnapshots(const fs::path& legacyValue) {
    fs::path legacy = FullPath(legacyValue);
    if (!fs::is_directory(legacy) || EqualsInsensitive(legacy.wstring(), rootDirectory_.wstring())) return 0;
    int imported = 0;
    for (const auto& entry : fs::recursive_directory_iterator(legacy, fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file() || entry.is_symlink() || entry.path().filename().wstring().find(L".tmp-") != std::wstring::npos) continue;
        fs::path relative = fs::relative(entry.path(), legacy);
        if (relative.empty() || *relative.begin() == L"..") continue;
        fs::path destination = rootDirectory_ / relative;
        if (fs::exists(destination)) continue;
        fs::create_directories(destination.parent_path());
        CopyFileAtomically(entry.path(), destination);
        ++imported;
    }
    return imported;
}

Snapshot SnapshotStore::CreateSnapshot(const fs::path& value, const std::wstring& processName,
                                       const fs::path& executable, std::optional<std::chrono::system_clock::time_point> created) {
    fs::path missionPath = NormalizeMissionPath(value, true);
    if (processName.empty() || executable.empty()) throw std::invalid_argument("Editor identity was incomplete.");
    auto sourceFiles = EnumerateMissionFamily(missionPath);
    if (std::none_of(sourceFiles.begin(), sourceFiles.end(), [&](const fs::path& p) { return EqualsInsensitive(p.wstring(), missionPath.wstring()); }))
        throw std::runtime_error("The mission file was not found in its companion file set.");
    auto timestamp = created.value_or(std::chrono::system_clock::now());
    fs::path directory = rootDirectory_ / BuildMissionKey(missionPath);
    fs::create_directories(directory);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()).count() % 1000;
    std::wostringstream stamp;
    stamp << FormatLocalSnapshotTime(timestamp) << L'-' << std::setw(3) << std::setfill(L'0') << millis;
    std::wstring proposed = SanitizeFileName(missionPath.stem().wstring()) + L"_" + stamp.str();
    std::wstring prefix = proposed;
    for (int suffix = 2; fs::exists(directory / (prefix + kMetadataSuffix)); ++suffix) prefix = proposed + L"_" + std::to_wstring(suffix);
    Snapshot snapshot{1, missionPath, processName, FullPath(executable), timestamp, {}, directory / (prefix + kMetadataSuffix), {}};
    std::vector<fs::path> copied;
    try {
        for (const auto& source : sourceFiles) {
            std::wstring storedName = prefix + source.extension().wstring();
            fs::path destination = directory / storedName;
            CopyFileAtomically(source, destination);
            copied.push_back(destination);
            snapshot.files.push_back({source.filename().wstring(), storedName, fs::file_size(destination), Sha256File(destination)});
        }
        WriteUtf8Atomically(snapshot.metadataPath, SerializeSnapshot(snapshot));
        EnforceRetention(directory);
        return snapshot;
    } catch (...) {
        std::error_code ignored;
        for (const auto& path : copied) fs::remove(path, ignored);
        fs::remove(snapshot.metadataPath, ignored);
        throw;
    }
}

Snapshot SnapshotStore::ReadMetadata(const fs::path& path) const {
    fs::path full = FullPath(path);
    if (!StartsWithInsensitive(full.wstring(), rootDirectory_.wstring() + L"\\")) throw std::runtime_error("Snapshot is outside the recovery folder.");
    auto json = JsonParser(ReadUtf8File(full)).Parse();
    if (json.type != JsonParser::Value::Type::Object) throw std::runtime_error("Autosave metadata root is invalid.");
    Snapshot snapshot;
    snapshot.schemaVersion = static_cast<int>(Member(json, "SchemaVersion", JsonParser::Value::Type::Number).number);
    snapshot.missionPath = Utf8ToWide(Member(json, "MissionPath", JsonParser::Value::Type::String).string);
    snapshot.editorProcessName = Utf8ToWide(Member(json, "EditorProcessName", JsonParser::Value::Type::String).string);
    snapshot.editorExecutablePath = Utf8ToWide(Member(json, "EditorExecutablePath", JsonParser::Value::Type::String).string);
    snapshot.createdUtc = ParseIsoTime(Member(json, "CreatedUtc", JsonParser::Value::Type::String).string);
    snapshot.metadataPath = full;
    for (const auto& item : Member(json, "Files", JsonParser::Value::Type::Array).array) {
        snapshot.files.push_back({
            Utf8ToWide(Member(item, "OriginalFileName", JsonParser::Value::Type::String).string),
            Utf8ToWide(Member(item, "SnapshotFileName", JsonParser::Value::Type::String).string),
            static_cast<std::uintmax_t>(Member(item, "Length", JsonParser::Value::Type::Number).number),
            Utf8ToWide(Member(item, "Sha256", JsonParser::Value::Type::String).string)});
    }
    ValidateSnapshot(snapshot);
    return snapshot;
}

void SnapshotStore::ValidateSnapshot(const Snapshot& snapshot) const {
    if (snapshot.schemaVersion != 1 || snapshot.files.empty() || !snapshot.missionPath.is_absolute() ||
        snapshot.editorProcessName.empty() || !snapshot.editorExecutablePath.is_absolute() || snapshot.metadataPath.empty())
        throw std::runtime_error("Autosave metadata has an unsupported or incomplete format.");
    std::set<std::wstring> originals, stored;
    for (const auto& file : snapshot.files) {
        ResolveMissionCompanionPath(snapshot.missionPath, file.originalFileName);
        ResolveChildPath(snapshot.metadataPath.parent_path(), file.snapshotFileName);
        std::wstring o = Lower(file.originalFileName), s = Lower(file.snapshotFileName);
        if (!originals.insert(o).second || !stored.insert(s).second || file.sha256.size() != 64)
            throw std::runtime_error("Autosave metadata contains an invalid file entry.");
    }
    if (!originals.contains(Lower(snapshot.missionPath.filename().wstring()))) throw std::runtime_error("Autosave metadata does not contain its .Mission file.");
}

Snapshot SnapshotStore::AssessIntegrity(Snapshot snapshot) const {
    try {
        for (const auto& file : snapshot.files) {
            fs::path stored = ResolveChildPath(snapshot.metadataPath.parent_path(), file.snapshotFileName);
            if (!fs::exists(stored)) { snapshot.integrityError = L"Missing file: " + file.originalFileName; return snapshot; }
            if (fs::file_size(stored) != file.length) { snapshot.integrityError = L"Incorrect file size: " + file.originalFileName; return snapshot; }
            if (!EqualsInsensitive(Sha256File(stored), file.sha256)) { snapshot.integrityError = L"Checksum mismatch: " + file.originalFileName; return snapshot; }
        }
    } catch (const std::exception& error) { snapshot.integrityError = L"Could not verify snapshot: " + Utf8ToWide(error.what()); }
    return snapshot;
}

std::vector<Snapshot> SnapshotStore::ListSnapshots(const std::optional<fs::path>& mission, int maximum) const {
    std::vector<Snapshot> result;
    if (!fs::is_directory(rootDirectory_) || maximum < 1) return result;
    std::optional<fs::path> normalized;
    if (mission) normalized = NormalizeMissionPath(*mission, false);
    for (const auto& entry : fs::recursive_directory_iterator(rootDirectory_, fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;
        std::wstring name = entry.path().filename().wstring();
        if (name.size() < std::size(kMetadataSuffix) - 1 || !EqualsInsensitive(name.substr(name.size() - (std::size(kMetadataSuffix) - 1)), kMetadataSuffix)) continue;
        try {
            Snapshot snapshot = ReadMetadata(entry.path());
            if (!normalized || EqualsInsensitive(snapshot.missionPath.wstring(), normalized->wstring())) result.push_back(AssessIntegrity(std::move(snapshot)));
        } catch (...) {}
    }
    std::sort(result.begin(), result.end(), [](const Snapshot& a, const Snapshot& b) { return a.createdUtc > b.createdUtc; });
    if (result.size() > static_cast<std::size_t>(maximum)) result.resize(static_cast<std::size_t>(maximum));
    return result;
}

std::size_t SnapshotStore::CountSnapshots() const {
    if (!fs::is_directory(rootDirectory_)) return 0;
    std::size_t count = 0;
    const std::size_t suffixLength = std::size(kMetadataSuffix) - 1;
    for (const auto& entry : fs::recursive_directory_iterator(rootDirectory_, fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;
        const std::wstring name = entry.path().filename().wstring();
        if (name.size() >= suffixLength && EqualsInsensitive(name.substr(name.size() - suffixLength), kMetadataSuffix)) ++count;
    }
    return count;
}

void SnapshotStore::EnforceRetention(const fs::path& directory) const {
    std::vector<Snapshot> snapshots;
    if (!fs::is_directory(directory)) return;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;
        try { snapshots.push_back(ReadMetadata(entry.path())); } catch (...) {}
    }
    std::sort(snapshots.begin(), snapshots.end(), [](const Snapshot& a, const Snapshot& b) { return a.createdUtc > b.createdUtc; });
    std::error_code ignored;
    for (std::size_t i = static_cast<std::size_t>(retentionCount_); i < snapshots.size(); ++i) {
        for (const auto& file : snapshots[i].files) fs::remove(ResolveChildPath(directory, file.snapshotFileName), ignored);
        fs::remove(snapshots[i].metadataPath, ignored);
    }
}

void SnapshotStore::PruneToRetentionLimit() {
    if (!fs::is_directory(rootDirectory_)) return;
    std::set<std::wstring> directories;
    for (const auto& entry : fs::recursive_directory_iterator(rootDirectory_, fs::directory_options::skip_permission_denied)) {
        if (entry.is_regular_file()) directories.insert(entry.path().parent_path().wstring());
    }
    for (const auto& directory : directories) EnforceRetention(directory);
}

RestoreResult SnapshotStore::RestoreSnapshot(const Snapshot& input) const {
    ValidateSnapshot(input);
    Snapshot snapshot = AssessIntegrity(input);
    if (!snapshot.IsRestorable()) throw std::runtime_error(WideToUtf8(snapshot.integrityError));
    fs::path missionPath = NormalizeMissionPath(snapshot.missionPath, false);
    fs::create_directories(missionPath.parent_path());
    fs::path recovery = rootDirectory_ / L"RecoveryBeforeRestore" / BuildMissionKey(missionPath) /
        (FormatLocalSnapshotTime(std::chrono::system_clock::now()) + L"_" + NewGuidText().substr(0, 6));
    fs::create_directories(recovery);
    auto current = EnumerateMissionFamily(missionPath);
    for (const auto& source : current) fs::copy_file(source, recovery / source.filename(), fs::copy_options::none);
    std::set<std::wstring> expected;
    std::vector<std::pair<fs::path, fs::path>> staged;
    const std::wstring operation = NewGuidText();
    try {
        for (const auto& file : snapshot.files) {
            fs::path destination = ResolveMissionCompanionPath(missionPath, file.originalFileName);
            fs::path stage = destination.wstring() + L".missionguard-restore-" + operation;
            fs::copy_file(ResolveChildPath(snapshot.metadataPath.parent_path(), file.snapshotFileName), stage, fs::copy_options::overwrite_existing);
            staged.emplace_back(stage, destination);
            expected.insert(Lower(file.originalFileName));
        }
        for (const auto& [stage, destination] : staged) {
            if (!MoveFileExW(stage.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Could not replace mission file: " + WideToUtf8(Win32ErrorMessage()));
        }
        for (const auto& file : current) if (!expected.contains(Lower(file.filename().wstring()))) fs::remove(file);
        return {missionPath, recovery, staged.size()};
    } catch (...) {
        std::error_code ignored;
        for (const auto& [stage, destination] : staged) { (void)destination; fs::remove(stage, ignored); }
        for (const auto& file : EnumerateMissionFamily(missionPath)) fs::remove(file, ignored);
        for (const auto& backup : fs::directory_iterator(recovery)) fs::copy_file(backup.path(), missionPath.parent_path() / backup.path().filename(), fs::copy_options::overwrite_existing);
        throw;
    }
}

}  // namespace missionguard
