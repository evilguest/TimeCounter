#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "json.h"
#include <cstdint>
#include <mutex>
#include <functional>
#include <memory>
#include <atomic>
#include <thread>

namespace tc {
using Time = int64_t;
constexpr Time Second = 10000000, Day = 86400 * Second;
void check(bool ok, const char* operation);
Time now();
Time parseTime(const std::string& text);
std::string isoTime(Time time);
std::wstring duration(double seconds);
std::wstring dataDirectory();
std::string readFile(const std::wstring& path);
void atomicWrite(const std::wstring& path, const std::string& data);
void logError(const std::string& text) noexcept;

struct Settings {
    int idleMinutes = 5, transparency = 75, offsetX = 20, offsetY = 20, monitor = 0, port = 47831;
    double fontSize = 144;
    uint32_t colorArgb = 0xffff0000;
    std::wstring fontName = L"Agency FB", corner = L"Справа сверху";
    std::string apiToken;
    bool overlayVisible = true;
    void validate() const;
    static Settings decode(const std::string& json);
    static Settings load();
    std::string encode() const;
    void save() const;
    COLORREF color() const { return RGB((colorArgb >> 16) & 255, (colorArgb >> 8) & 255, colorArgb & 255); }
};
bool autoStart();
void setAutoStart(bool enable);
std::wstring executablePath();
struct Segment { Time start, end; bool active; };
struct DayRow { std::string date; double active, inactive, uncounted, elapsed; };
class Ledger {
    std::wstring path;
    mutable std::mutex gate;
    std::vector<Segment> segments;
public:
    explicit Ledger(const std::wstring& path);
    void add(Time start, Time end, bool active);
    void save();
    std::vector<DayRow> history(int previousDays, Time until, const DYNAMIC_TIME_ZONE_INFORMATION& zone) const;
};
DYNAMIC_TIME_ZONE_INFORMATION localZone();
std::string historyJson(const Ledger& ledger, int days, Time until);
void record(Ledger& ledger, Time from, Time to, bool available, Time input, Time timeout);
Time lastInput(Time time);
bool sessionAvailable();

struct HttpResponse { int status; std::string body; };
HttpResponse handleRequest(const std::string& headers, const std::string& token, const std::function<std::string(int)>& history);
class ApiServer {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    ApiServer(int port, const std::string& token, std::function<std::string(int)> history);
    ~ApiServer();
    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;
};
}
