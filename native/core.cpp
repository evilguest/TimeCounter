#include "core.h"
#include <wtsapi32.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
#include <limits>

namespace tc {
void check(bool ok, const char* op) { if (!ok) throw std::runtime_error(std::string(op) + " (Windows error " + std::to_string(GetLastError()) + ")"); }
static Time fromFile(const FILETIME& f) { return (Time(f.dwHighDateTime) << 32) | f.dwLowDateTime; }
static FILETIME toFile(Time t) { return FILETIME{DWORD(t), DWORD(uint64_t(t) >> 32)}; }
static Time fromSystem(const SYSTEMTIME& s) { FILETIME f; check(SystemTimeToFileTime(&s, &f) != 0, "SystemTimeToFileTime"); return fromFile(f); }
static SYSTEMTIME toSystem(Time t) { auto f = toFile(t); SYSTEMTIME s{}; check(FileTimeToSystemTime(&f, &s) != 0, "FileTimeToSystemTime"); return s; }
Time now() { FILETIME f; GetSystemTimePreciseAsFileTime(&f); return fromFile(f); }
Time parseTime(const std::string& s) {
    auto bad = []() -> void { throw std::runtime_error("Invalid history timestamp"); };
    if (s.size() < 20) bad();
    auto digits = [&](size_t at, size_t n) {
        int result = 0; if (at + n > s.size()) bad();
        for (size_t i = at; i < at + n; ++i) { if (s[i] < '0' || s[i] > '9') bad(); result = result * 10 + s[i] - '0'; } return result;
    };
    if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':') bad();
    SYSTEMTIME date{}; date.wYear = WORD(digits(0, 4)); date.wMonth = WORD(digits(5, 2)); date.wDay = WORD(digits(8, 2));
    date.wHour = WORD(digits(11, 2)); date.wMinute = WORD(digits(14, 2)); date.wSecond = WORD(digits(17, 2));
    if (date.wYear < 1601 || date.wYear > 9999 || date.wHour > 23 || date.wMinute > 59 || date.wSecond > 59) bad();
    Time t = fromSystem(date); auto back = toSystem(t);
    if (back.wYear != date.wYear || back.wMonth != date.wMonth || back.wDay != date.wDay) bad();
    size_t p = 19; Time fraction = 0;
    if (s[p] == '.') { size_t begin = ++p; while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
        size_t n = p - begin; if (!n || n > 7) bad(); fraction = digits(begin, n); for (; n < 7; ++n) fraction *= 10;
    }
    int offset = 0;
    if (p < s.size() && s[p] == 'Z') { if (++p != s.size()) bad(); }
    else {
        if (s.size() - p != 6 || (s[p] != '+' && s[p] != '-') || s[p + 3] != ':') bad();
        int hours = digits(p + 1, 2), minutes = digits(p + 4, 2);
        if (hours > 14 || minutes > 59 || (hours == 14 && minutes != 0)) bad();
        offset = (hours * 60 + minutes) * (s[p] == '-' ? -1 : 1); p += 6;
    }
    return t + fraction - Time(offset) * 60 * Second;
}
std::string isoTime(Time t) {
    auto s = toSystem(t); char b[64];
    sprintf_s(b, "%04u-%02u-%02uT%02u:%02u:%02u.%07lld+00:00", s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute, s.wSecond, t % Second); return b;
}
std::wstring duration(double seconds) {
    auto minutes = int64_t(std::max(0.0, seconds)) / 60; wchar_t b[48]; swprintf_s(b, L"%02lld:%02lld", minutes / 60, minutes % 60); return b;
}
std::wstring dataDirectory() {
    PWSTR p = nullptr; check(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)), "LocalAppData");
    std::wstring result(p); CoTaskMemFree(p); result += L"\\TimeCounter";
    check(CreateDirectoryW(result.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS, "Create data directory"); return result;
}
std::string readFile(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(f != INVALID_HANDLE_VALUE, "Open history");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart > 256 * 1024 * 1024) { CloseHandle(f); throw std::runtime_error("History exceeds 256 MiB or cannot be read"); }
    std::string result(size_t(size.QuadPart), 0); DWORD read = 0;
    bool ok = ReadFile(f, result.data(), (DWORD)result.size(), &read, nullptr) && read == result.size(); CloseHandle(f); check(ok, "Read history"); return result;
}
void atomicWrite(const std::wstring& path, const std::string& data) {
    auto temp = path + L".tmp", backup = path + L".bak";
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); check(f != INVALID_HANDLE_VALUE, "Create temporary history");
    DWORD written = 0; bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size();
    if (ok) ok = FlushFileBuffers(f) != 0; CloseHandle(f); check(ok, "Write history");
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) check(ReplaceFileW(path.c_str(), temp.c_str(), backup.c_str(), 0, nullptr, nullptr) != 0, "Replace history");
    else check(MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != 0, "Commit history");
}
void logError(const std::string& text) noexcept {
    try {
        auto p = dataDirectory() + L"\\errors.log";
        HANDLE f = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        auto message = isoTime(now()) + " " + text + "\r\n"; DWORD n; WriteFile(f, message.data(), (DWORD)message.size(), &n, nullptr); CloseHandle(f);
    } catch (...) { }
}
static std::wstring regRead(const wchar_t* key, const wchar_t* value) {
    DWORD bytes = 0; LSTATUS code = RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return {};
    if (code != ERROR_SUCCESS || bytes > 1024 * 1024) throw std::runtime_error("Cannot read registry settings");
    std::wstring s(bytes / sizeof(wchar_t), 0); code = RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_SZ, nullptr, s.data(), &bytes);
    if (code != ERROR_SUCCESS) throw std::runtime_error("Cannot read registry settings");
    while (!s.empty() && !s.back()) s.pop_back(); return s;
}
static void regWrite(const wchar_t* path, const wchar_t* value, const std::wstring& text) {
    HKEY key; LSTATUS code = RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (code != ERROR_SUCCESS) throw std::runtime_error("Cannot create registry key");
    code = RegSetValueExW(key, value, 0, REG_SZ, (const BYTE*)text.c_str(), DWORD((text.size() + 1) * sizeof(wchar_t))); RegCloseKey(key);
    if (code != ERROR_SUCCESS) throw std::runtime_error("Cannot save registry settings");
}
void Settings::validate() const {
    if (idleMinutes < 1 || idleMinutes > 1440 || port < 1024 || port > 65535 || !std::isfinite(fontSize) || fontSize < 8 || fontSize > 500 || transparency < 0 || transparency > 99 || apiToken.size() < 16 || apiToken.size() > 1024 || fontName.empty() || fontName.size() >= LF_FACESIZE || offsetX < 0 || offsetX > 10000 || offsetY < 0 || offsetY > 10000 || monitor < 0 || monitor > 255)
        throw std::runtime_error("Некорректные настройки TimeCounter");
    for (unsigned char c : apiToken) if (c < 33 || c > 126) throw std::runtime_error("Invalid API token");
    if (corner != L"Справа сверху" && corner != L"Слева сверху" && corner != L"Справа снизу" && corner != L"Слева снизу") throw std::runtime_error("Invalid overlay corner");
}
Settings Settings::decode(const std::string& text) {
    auto j = parseJson(text); if (j.type != Json::Object) throw std::runtime_error("Settings must be a JSON object"); Settings s;
    auto integer = [&](const char* k, int& v) { if (j.object.count(k)) { double n = j.at(k).num(); if (n != std::floor(n) || n < INT_MIN || n > INT_MAX) throw std::runtime_error("Invalid integer setting"); v = (int)n; } };
    integer("IdleMinutes", s.idleMinutes); integer("Transparency", s.transparency); integer("OffsetX", s.offsetX); integer("OffsetY", s.offsetY); integer("Monitor", s.monitor); integer("Port", s.port);
    if (j.object.count("FontSize")) s.fontSize = j.at("FontSize").num();
    if (j.object.count("ColorArgb")) { double n = j.at("ColorArgb").num(); if (n != std::floor(n) || n < INT_MIN || n > INT_MAX) throw std::runtime_error("Invalid color"); s.colorArgb = uint32_t(int32_t(n)); }
    if (j.object.count("FontName")) s.fontName = wide(j.at("FontName").str());
    if (j.object.count("Corner")) s.corner = wide(j.at("Corner").str());
    if (j.object.count("ApiToken")) s.apiToken = j.at("ApiToken").str();
    if (j.object.count("OverlayVisible")) s.overlayVisible = j.at("OverlayVisible").flag();
    s.validate(); return s;
}
Settings Settings::load() {
    auto text = regRead(L"Software\\TimeCounter", L"Settings"); if (!text.empty()) return decode(utf8(text));
    Settings s; BYTE bytes[24]; if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) throw std::runtime_error("Cannot generate API token");
    const char* hex = "0123456789ABCDEF"; for (BYTE b : bytes) { s.apiToken += hex[b >> 4]; s.apiToken += hex[b & 15]; } return s;
}
std::string Settings::encode() const {
    return "{\"IdleMinutes\":" + std::to_string(idleMinutes) + ",\"FontName\":" + quote(utf8(fontName)) + ",\"FontSize\":" + std::to_string(fontSize) + ",\"ColorArgb\":" + std::to_string(int32_t(colorArgb)) + ",\"Transparency\":" + std::to_string(transparency) + ",\"Corner\":" + quote(utf8(corner)) + ",\"OffsetX\":" + std::to_string(offsetX) + ",\"OffsetY\":" + std::to_string(offsetY) + ",\"Monitor\":" + std::to_string(monitor) + ",\"Port\":" + std::to_string(port) + ",\"ApiToken\":" + quote(apiToken) + ",\"OverlayVisible\":" + (overlayVisible ? "true" : "false") + "}";
}
void Settings::save() const { validate(); regWrite(L"Software\\TimeCounter", L"Settings", wide(encode())); }
bool autoStart() { return !regRead(L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"TimeCounter").empty(); }
std::wstring executablePath() { std::wstring p(32768, 0); DWORD n = GetModuleFileNameW(nullptr, p.data(), (DWORD)p.size()); check(n && n < p.size(), "Executable path"); p.resize(n); return p; }
void setAutoStart(bool enabled) {
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (enabled) regWrite(key, L"TimeCounter", L"\"" + executablePath() + L"\"");
    else { LSTATUS code = RegDeleteKeyValueW(HKEY_CURRENT_USER, key, L"TimeCounter"); if (code != ERROR_SUCCESS && code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) throw std::runtime_error("Cannot disable auto-start"); }
}
Ledger::Ledger(const std::wstring& file) : path(file) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) { DWORD e = GetLastError(); if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return; check(false, "Inspect history"); }
    try {
        auto json = parseJson(readFile(path)); if (json.type != Json::Array) throw std::runtime_error("History must be an array");
        for (const auto& j : json.array) {
            Segment s{parseTime(j.at("Start").str()), parseTime(j.at("End").str()), j.at("Active").flag()};
            if (s.end <= s.start || (!segments.empty() && s.start < segments.back().end)) throw std::runtime_error("Overlapping or invalid history intervals");
            segments.push_back(s);
        }
    } catch (const std::exception& e) { throw std::runtime_error("История не изменена: " + utf8(path) + "\n" + e.what()); }
}
void Ledger::add(Time a, Time b, bool active) {
    std::lock_guard<std::mutex> lock(gate);
    if (!segments.empty()) a = std::max(a, segments.back().end);
    if (b <= a) return;
    if (!segments.empty() && segments.back().active == active && segments.back().end == a) segments.back().end = b;
    else segments.push_back({a, b, active});
}
void Ledger::save() {
    std::lock_guard<std::mutex> lock(gate); std::string data = "[";
    for (const auto& s : segments) { if (data.size() > 1) data += ','; data += "{\"Start\":" + quote(isoTime(s.start)) + ",\"End\":" + quote(isoTime(s.end)) + ",\"Active\":" + (s.active ? "true" : "false") + "}"; }
    data += ']'; atomicWrite(path, data);
}
DYNAMIC_TIME_ZONE_INFORMATION localZone() { DYNAMIC_TIME_ZONE_INFORMATION z{}; check(GetDynamicTimeZoneInformation(&z) != TIME_ZONE_ID_INVALID, "Local timezone"); return z; }
static Time boundary(Time localDate, const DYNAMIC_TIME_ZONE_INFORMATION& z) {
    // Resolve a midnight skipped by a DST transition at its first valid minute.
    for (int minute = 0; minute < 1440; ++minute) {
        auto local = toSystem(localDate + Time(minute) * 60 * Second); SYSTEMTIME utc{}, roundtrip{};
        if (TzSpecificLocalTimeToSystemTimeEx(&z, &local, &utc) && SystemTimeToTzSpecificLocalTimeEx(&z, &utc, &roundtrip) && roundtrip.wYear == local.wYear && roundtrip.wMonth == local.wMonth && roundtrip.wDay == local.wDay && roundtrip.wHour == local.wHour && roundtrip.wMinute == local.wMinute) return fromSystem(utc);
    }
    throw std::runtime_error("Local date has no valid midnight boundary");
}
std::vector<DayRow> Ledger::history(int days, Time until, const DYNAMIC_TIME_ZONE_INFORMATION& zone) const {
    if (days < 0 || days > 3660) throw std::runtime_error("Invalid history range");
    auto utc = toSystem(until); SYSTEMTIME local{}; check(SystemTimeToTzSpecificLocalTimeEx(&zone, &utc, &local) != 0, "Local time");
    local.wHour = local.wMinute = local.wSecond = local.wMilliseconds = 0; Time today = fromSystem(local);
    std::lock_guard<std::mutex> lock(gate); std::vector<DayRow> result;
    for (int i = 0; i <= days; ++i) {
        Time localDate = today - Time(i) * Day; auto d = toSystem(localDate); char date[16]; sprintf_s(date, "%04u-%02u-%02u", d.wYear, d.wMonth, d.wDay);
        Time begin = boundary(localDate, zone), end = std::min(until, boundary(localDate + Day, zone));
        DayRow row{date, 0, 0, 0, std::max(0.0, double(end - begin) / Second)};
        auto first = std::lower_bound(segments.begin(), segments.end(), begin, [](const Segment& s, Time t) { return s.end < t; });
        for (auto it = first; it != segments.end(); ++it) {
            const auto& s = *it;
            if (s.start >= end) break;
            Time a = std::max(begin, s.start), b = std::min(end, s.end);
            if (b > a) (s.active ? row.active : row.inactive) += double(b - a) / Second;
        }
        row.uncounted = std::max(0.0, row.elapsed - row.active - row.inactive); result.push_back(row);
    } return result;
}
static std::string seconds(double n) { char b[48]; sprintf_s(b, "%.7f", n); return b; }
std::string historyJson(const Ledger& ledger, int days, Time until) {
    auto zone = localZone(); std::string z = utf8(zone.TimeZoneKeyName);
    if (z.empty()) z = utf8(zone.StandardName);
    std::string json = "{\"generatedAt\":" + quote(isoTime(until)) + ",\"timeZone\":" + quote(z) + ",\"days\":[";
    bool first = true;
    for (const auto& row : ledger.history(days, until, zone)) {
        if (!first) json += ','; first = false;
        json += "{\"date\":" + quote(row.date) + ",\"activeSeconds\":" + seconds(row.active) + ",\"inactiveSeconds\":" + seconds(row.inactive) + ",\"uncountedSeconds\":" + seconds(row.uncounted) + ",\"elapsedSeconds\":" + seconds(row.elapsed) + "}";
    } return json + "]}";
}
void record(Ledger& ledger, Time from, Time to, bool available, Time input, Time timeout) {
    if (to <= from) return;
    if (!available) { ledger.add(from, to, false); return; }
    Time split = std::min(to, input + timeout);
    if (split > from) ledger.add(from, split, true);
    if (to > split) ledger.add(std::max(from, split), to, false);
}
Time lastInput(Time time) { LASTINPUTINFO info{sizeof(info), 0}; check(GetLastInputInfo(&info) != 0, "GetLastInputInfo"); return time - Time(DWORD(GetTickCount64()) - info.dwTime) * 10000; }
bool sessionAvailable() {
    DWORD session = 0; if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) return false;
    LPWSTR buffer = nullptr; DWORD bytes;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSConnectState, &buffer, &bytes)) return false;
    bool active = bytes >= sizeof(WTS_CONNECTSTATE_CLASS) && *(WTS_CONNECTSTATE_CLASS*)buffer == WTSActive; WTSFreeMemory(buffer);
    if (!active) return false;
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP); if (!desktop) return false; CloseDesktop(desktop); return true;
}
}
