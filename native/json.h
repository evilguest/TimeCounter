#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <stdexcept>
#include <cmath>
#include <cstdlib>

namespace tc {
inline std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (!n) throw std::runtime_error("Invalid UTF-8");
    std::wstring r(n, 0); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), r.data(), n); return r;
}
inline std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (!n) throw std::runtime_error("Invalid UTF-16");
    std::string r(n, 0); WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), (int)s.size(), r.data(), n, nullptr, nullptr); return r;
}
inline std::string quote(const std::string& s) {
    std::string r = "\""; const char* hex = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { r += '\\'; r += c; }
        else if (c < 32) { r += "\\u00"; r += hex[c >> 4]; r += hex[c & 15]; }
        else r += c;
    }
    return r + '"';
}
struct Json {
    enum Type { Null, Boolean, Number, String, Array, Object } type = Null;
    bool boolean = false; double number = 0;
    std::string string; std::vector<Json> array; std::map<std::string, Json> object;
    const Json& at(const std::string& key) const {
        if (type != Object || !object.count(key)) throw std::runtime_error("Missing JSON field: " + key);
        return object.at(key);
    }
    std::string str() const { if (type != String) throw std::runtime_error("Expected JSON string"); return string; }
    bool flag() const { if (type != Boolean) throw std::runtime_error("Expected JSON boolean"); return boolean; }
    double num() const { if (type != Number) throw std::runtime_error("Expected JSON number"); return number; }
};
class JsonParser {
    const std::string& s; size_t p = 0;
    [[noreturn]] void fail() const { throw std::runtime_error("Invalid JSON at byte " + std::to_string(p)); }
    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p; }
    bool eat(char c) { ws(); if (p < s.size() && s[p] == c) { ++p; return true; } return false; }
    unsigned hex4() {
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            if (p >= s.size()) fail(); unsigned char c = s[p++]; unsigned d;
            if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10; else fail();
            v = v * 16 + d;
        } return v;
    }
    std::string text() {
        if (!eat('"')) fail(); std::string r;
        while (p < s.size()) {
            unsigned char c = s[p++]; if (c == '"') { wide(r); return r; }
            if (c < 32) fail();
            if (c != '\\') { r += c; continue; }
            if (p == s.size()) fail(); c = s[p++];
            switch (c) {
            case '"': case '\\': case '/': r += c; break;
            case 'b': r += '\b'; break; case 'f': r += '\f'; break;
            case 'n': r += '\n'; break; case 'r': r += '\r'; break; case 't': r += '\t'; break;
            case 'u': {
                unsigned u = hex4(); std::wstring w(1, (wchar_t)u);
                if (u >= 0xd800 && u <= 0xdbff) {
                    if (p + 2 > s.size() || s[p++] != '\\' || s[p++] != 'u') fail();
                    unsigned lo = hex4(); if (lo < 0xdc00 || lo > 0xdfff) fail(); w += (wchar_t)lo;
                } else if (u >= 0xdc00 && u <= 0xdfff) fail();
                r += utf8(w); break;
            }
            default: fail();
            }
        } fail();
    }
    Json value(unsigned depth) {
        if (depth > 64) fail(); ws(); if (p == s.size()) fail(); Json j;
        if (s[p] == '"') { j.type = Json::String; j.string = text(); }
        else if (eat('{')) {
            j.type = Json::Object; if (eat('}')) return j;
            do { std::string k = text(); if (!eat(':')) fail(); auto v = value(depth + 1); if (!j.object.emplace(k, std::move(v)).second) fail(); } while (eat(','));
            if (!eat('}')) fail();
        } else if (eat('[')) {
            j.type = Json::Array; if (eat(']')) return j;
            do { j.array.push_back(value(depth + 1)); } while (eat(',')); if (!eat(']')) fail();
        } else if (s.compare(p, 4, "true") == 0) { j.type = Json::Boolean; j.boolean = true; p += 4; }
        else if (s.compare(p, 5, "false") == 0) { j.type = Json::Boolean; p += 5; }
        else if (s.compare(p, 4, "null") == 0) p += 4;
        else {
            size_t begin = p; if (s[p] == '-') ++p;
            if (p >= s.size()) fail();
            if (s[p] == '0') ++p;
            else { if (s[p] < '1' || s[p] > '9') fail(); while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p; }
            if (p < s.size() && s[p] == '.') { ++p; size_t a = p; while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p; if (p == a) fail(); }
            if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) { ++p; if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p; size_t a = p; while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p; if (p == a) fail(); }
            j.type = Json::Number; j.number = std::strtod(s.substr(begin, p - begin).c_str(), nullptr); if (!std::isfinite(j.number)) fail();
        } return j;
    }
public:
    explicit JsonParser(const std::string& source) : s(source) { if (s.compare(0, 3, "\xef\xbb\xbf") == 0) p = 3; }
    Json parse() { auto j = value(0); ws(); if (p != s.size()) fail(); return j; }
};
inline Json parseJson(const std::string& text) { return JsonParser(text).parse(); }
}
