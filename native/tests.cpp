#include <winsock2.h>
#include <ws2tcpip.h>
#include "core.h"
#include <iostream>
#include <algorithm>

namespace tc { void runUiTests(const std::wstring& folder, int port); }
using namespace tc;
static void expect(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void equal(double a, double b, const char* message) { expect(std::abs(a - b) < .000001, message); }
template<class F> static void rejects(F f, const char* message) { bool threw = false; try { f(); } catch (const std::exception&) { threw = true; } expect(threw, message); }
static int freePort() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); expect(s != INVALID_SOCKET, "test socket"); sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    expect(bind(s, (sockaddr*)&a, sizeof(a)) == 0, "test bind"); int n = sizeof(a); expect(getsockname(s, (sockaddr*)&a, &n) == 0, "test socket address"); int port = ntohs(a.sin_port); closesocket(s); return port;
}
static std::string http(int port, const std::string& path, const std::string& token, bool ipv6 = false) {
    SOCKET s = socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP); expect(s != INVALID_SOCKET, "HTTP test socket");
    DWORD timeout = 5000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout)); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sockaddr_in6 v6{}; v6.sin6_family = AF_INET6; v6.sin6_port = htons((u_short)port); v6.sin6_addr = in6addr_loopback;
    if (connect(s, ipv6 ? (sockaddr*)&v6 : (sockaddr*)&a, ipv6 ? sizeof(v6) : sizeof(a))) { closesocket(s); throw std::runtime_error("HTTP connect"); }
    std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer " + token + "\r\n\r\n";
    if (send(s, request.data(), (int)request.size(), 0) != request.size()) { closesocket(s); throw std::runtime_error("HTTP send"); }
    std::string response; char buffer[4096];
    for (;;) { int n = recv(s, buffer, sizeof(buffer), 0); if (!n) break; if (n < 0) { closesocket(s); throw std::runtime_error("HTTP receive"); } response.append(buffer, n); }
    closesocket(s); return response;
}
int main() {
    std::wstring folder; WSADATA data; bool ws = false;
    try {
        expect(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Winsock"); ws = true;
        wchar_t temp[32768]; expect(GetTempPathW(32768, temp) != 0, "Temp directory");
        folder = std::wstring(temp) + L"TimeCounter-NativeTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        expect(CreateDirectoryW(folder.c_str(), nullptr) != 0, "Create isolated test directory");
        Time time = parseTime("2026-11-12T12:00:00+00:00");
        expect(isoTime(parseTime("2026-11-12T19:00:00.1234567+07:00")) == "2026-11-12T12:00:00.1234567+00:00", "legacy offset and 100ns precision");
        expect(parseTime("2026-11-12T12:00:00Z") == time, "UTC Z timestamps");
        rejects([] { parseTime("2026-02-31T00:00:00Z"); }, "invalid calendar date must fail");
        rejects([] { parseTime("2026-11-12T24:00:00Z"); }, "invalid hour must fail");
        expect(parseJson("{\"name\":\"\\u0421\\u043f\\u0440\\u0430\\u0432\\u0430 \\u0441\\u0432\\u0435\\u0440\\u0445\\u0443\"}").at("name").str() == utf8(L"Справа сверху"), ".NET unicode escapes");
        expect(wide(parseJson("\"\\ud83d\\udd52\"").str()).size() == 2, "JSON surrogate pairs");
        for (const auto& bad : {"[1,]", "{\"a\":1,\"a\":2}", "\"\\ud800\"", "01", "1e999", "true false", "\"x\n\""}) rejects([&] { parseJson(bad); }, "invalid JSON must fail");
        Settings settings; settings.apiToken = "legacy-test-token-0123456789";
        auto legacy = settings.encode(); auto restored = Settings::decode(legacy);
        expect(restored.colorArgb == 0xffff0000 && restored.color() == RGB(255, 0, 0) && restored.corner == L"Справа сверху" && restored.fontName == L"Agency FB" && restored.apiToken == settings.apiToken, "legacy settings roundtrip");
        settings.fontName = L"Segoe UI"; settings.fontSize = 143.5; settings.transparency = 73; settings.overlayVisible = false;
        restored = Settings::decode(settings.encode()); expect(restored.fontSize == 143.5 && !restored.overlayVisible && restored.transparency == 73, "custom settings roundtrip");
        rejects([] { Settings::decode("{\"ApiToken\":\"0123456789abcdef\",\"IdleMinutes\":0}"); }, "invalid settings must fail");
        expect(duration(3599.999) == L"00:59" && duration(3600) == L"01:00" && duration(90061) == L"25:01", "HH:mm format");
        DYNAMIC_TIME_ZONE_INFORMATION utc{}; utc.DynamicDaylightTimeDisabled = TRUE; wcscpy_s(utc.StandardName, L"UTC"); wcscpy_s(utc.TimeZoneKeyName, L"UTC");
        auto file = folder + L"\\history.json"; Ledger ledger(file); Time start = time - 600 * Second;
        record(ledger, start, time, true, start, 300 * Second); auto row = ledger.history(0, time, utc).front();
        equal(300, row.active, "idle deadline"); equal(300, row.inactive, "idle remainder"); equal(43200, row.active + row.inactive + row.uncounted, "day partition");
        ledger.save(); auto original = readFile(file); Ledger migrated(file); expect(readFile(file) == original, "loading does not rewrite legacy history");
        equal(300, migrated.history(0, time, utc).front().active, "history reload"); migrated.save(); expect(GetFileAttributesW((file + L".bak").c_str()) != INVALID_FILE_ATTRIBUTES, "atomic backup");
        Ledger recovery(folder + L"\\recovery.json");
        record(recovery, start, time - 2 * Second, true, start, 300 * Second); record(recovery, time - 2 * Second, time, true, time - 2 * Second, 300 * Second);
        equal(302, recovery.history(0, time, utc).front().active, "idle recovery does not backfill gap");
        Ledger midnight(folder + L"\\midnight.json"); Time boundary = parseTime("2026-11-12T00:00:00Z"); midnight.add(boundary - 60 * Second, boundary + 60 * Second, true);
        auto rows = midnight.history(1, time, utc); equal(60, rows[0].active, "midnight today"); equal(60, rows[1].active, "midnight yesterday");
        Ledger disconnected(folder + L"\\disconnected.json"); record(disconnected, start, time, false, time, 300 * Second); equal(0, disconnected.history(0, time, utc).front().active, "disconnected");
        Ledger overlap(folder + L"\\overlap.json"); overlap.add(time - 120 * Second, time, true); overlap.add(time - 60 * Second, time + 60 * Second, true);
        equal(180, overlap.history(0, time + 60 * Second, utc).front().active, "clock rollback overlap");
        DYNAMIC_TIME_ZONE_INFORMATION eastern{}; bool found = false;
        for (DWORD i = 0; EnumDynamicTimeZoneInformation(i, &eastern) == ERROR_SUCCESS; ++i) if (std::wstring(eastern.TimeZoneKeyName) == L"Eastern Standard Time") { found = true; break; }
        expect(found, "Eastern timezone installed"); eastern.DynamicDaylightTimeDisabled = FALSE;
        equal(23 * 3600, ledger.history(1, parseTime("2026-03-09T12:00:00Z"), eastern)[1].elapsed, "spring DST");
        equal(25 * 3600, ledger.history(1, parseTime("2026-11-02T12:00:00Z"), eastern)[1].elapsed, "fall DST");
        DYNAMIC_TIME_ZONE_INFORMATION novosibirsk{}; found = false;
        for (DWORD i = 0; EnumDynamicTimeZoneInformation(i, &novosibirsk) == ERROR_SUCCESS; ++i) if (std::wstring(novosibirsk.TimeZoneKeyName) == L"N. Central Asia Standard Time") { found = true; break; }
        expect(found, "Novosibirsk timezone installed");
        Ledger localMidnight(folder + L"\\local-midnight.json"); Time localBoundary = parseTime("2026-11-13T00:00:00+07:00");
        localMidnight.add(localBoundary - 3600 * Second, localBoundary, true);
        auto atMidnight = localMidnight.history(1, localBoundary, novosibirsk);
        expect(atMidnight[0].date == "2026-11-13" && atMidnight[1].date == "2026-11-12", "daily reset follows local date, not UTC");
        equal(0, atMidnight[0].active, "today resets to zero exactly at local midnight"); equal(3600, atMidnight[1].active, "yesterday retained in history");
        localMidnight.add(localBoundary, localBoundary + 60 * Second, true);
        equal(60, localMidnight.history(0, localBoundary + 60 * Second, novosibirsk)[0].active, "today excludes yesterday's work");
        auto corrupt = folder + L"\\corrupt.json"; atomicWrite(corrupt, "[{broken"); rejects([&] { Ledger failed(corrupt); }, "corrupt history fails closed"); expect(readFile(corrupt) == "[{broken", "corrupt history preserved");
        const std::string token = "test-token-0123456789";
        auto callback = [&](int days) { return historyJson(ledger, days, time); };
        auto request = [&](const std::string& target, const std::string& auth = "test-token-0123456789", const std::string& method = "GET") { return handleRequest(method + " " + target + " HTTP/1.1\r\nAuthorization: Bearer " + auth + "\r\n\r\n", token, callback); };
        expect(request("/api/history").status == 200, "API default"); expect(request("/api/history", "wrong").status == 401, "API auth");
        expect(request("/api/history", token, "POST").status == 405, "API method"); expect(request("/other").status == 404, "API route");
        expect(parseJson(request("/api/history?days=7").body).at("days").array.size() == 8, "API previous days");
        for (auto query : {"days=-1", "days=3661", "days=x", "days=", "x=1", "days=1&x=2", "days=99999999999999999"}) expect(request(std::string("/api/history?") + query).status == 400, "API invalid range");
        expect(handleRequest(std::string(8192, 'x'), token, callback).status == 431, "API header bound");
        int port = freePort();
        {
            ApiServer api(port, token, callback);
            rejects([&] { ApiServer conflict(port, token, callback); }, "port conflict");
            auto response = http(port, "/api/history?days=7", token); expect(response.find("HTTP/1.1 200 ") == 0, "IPv4 HTTP status");
            auto json = response.substr(response.find("\r\n\r\n") + 4); expect(parseJson(json).at("days").array.size() == 8, "HTTP JSON payload");
            expect(http(port, "/api/history", "wrong").find("HTTP/1.1 401 ") == 0, "HTTP auth");
            expect(http(port, "/api/history", token, true).find("HTTP/1.1 200 ") == 0, "IPv6 dual-stack");
            std::vector<std::thread> clients; std::atomic<int> success{0};
            for (int i = 0; i < 8; ++i) clients.emplace_back([&] { try { if (http(port, "/api/history", token).find("HTTP/1.1 200 ") == 0) ++success; } catch (...) { } });
            for (auto& c : clients) c.join(); expect(success == 8, "concurrent clients");
        }
        runUiTests(folder, freePort());
        WIN32_FIND_DATAW entry; HANDLE search = FindFirstFileW((folder + L"\\*").c_str(), &entry);
        if (search != INVALID_HANDLE_VALUE) { do { if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((folder + L"\\" + entry.cFileName).c_str()); } while (FindNextFileW(search, &entry)); FindClose(search); }
        RemoveDirectoryW(folder.c_str()); WSACleanup();
        std::cout << "PASS: legacy JSON/settings and 100ns dates; accounting/idle recovery/disconnect/midnight/DST/clock rollback; atomic persistence/corruption; HTTP auth/routes/ranges/IPv4/IPv6/concurrency; real tray clicks, settings, history, overlay and shutdown\n"; return 0;
    } catch (const std::exception& e) { if (ws) WSACleanup(); std::cerr << "FAIL: " << e.what() << "\nArtifacts: " << utf8(folder) << '\n'; return 1; }
}
