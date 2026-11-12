#include <winsock2.h>
#include <ws2tcpip.h>
#include "core.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <sstream>

namespace tc {
static std::string trim(std::string s) { while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back(); size_t n = s.find_first_not_of(" \t"); return n == std::string::npos ? "" : s.substr(n); }
static bool equalToken(const std::string& a, const std::string& b) {
    size_t different = a.size() ^ b.size();
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) different |= (i < a.size() ? (unsigned char)a[i] : 0) ^ (i < b.size() ? (unsigned char)b[i] : 0);
    return different == 0;
}
HttpResponse handleRequest(const std::string& request, const std::string& token, const std::function<std::string(int)>& history) {
    auto error = [](int status, const char* text) { return HttpResponse{status, "{\"error\":" + quote(text) + "}"}; };
    if (request.size() >= 8192) return error(431, "Headers too large");
    size_t end = request.find("\r\n"); if (end == std::string::npos) return error(400, "Invalid request");
    std::istringstream first(request.substr(0, end)); std::string method, target, version, extra;
    if (!(first >> method >> target >> version) || (first >> extra) || (version != "HTTP/1.0" && version != "HTTP/1.1")) return error(400, "Invalid request line");
    std::string auth; bool hasAuth = false;
    size_t p = end + 2;
    while (p < request.size()) {
        end = request.find("\r\n", p); if (end == std::string::npos) return error(400, "Invalid headers");
        if (end == p) break;
        auto line = request.substr(p, end - p); size_t colon = line.find(':'); if (colon == std::string::npos) return error(400, "Invalid header");
        auto key = line.substr(0, colon); for (char& c : key) c = (char)std::tolower((unsigned char)c);
        if (key == "authorization") { if (hasAuth) return error(400, "Duplicate authorization header"); hasAuth = true; auth = trim(line.substr(colon + 1)); }
        p = end + 2;
    }
    if (!equalToken(auth, "Bearer " + token)) return error(401, "Bearer token required");
    if (method != "GET") return error(405, "Use GET");
    size_t q = target.find('?'); auto path = target.substr(0, q);
    if (path != "/api/history") return error(404, "Use /api/history?days=0");
    int days = 0;
    if (q != std::string::npos && q + 1 != target.size()) {
        auto query = target.substr(q + 1); if (query.compare(0, 5, "days=") != 0 || query.size() == 5) return error(400, "days must be 0..3660");
        for (size_t i = 5; i < query.size(); ++i) { if (query[i] < '0' || query[i] > '9') return error(400, "days must be 0..3660"); days = days * 10 + query[i] - '0'; if (days > 3660) return error(400, "days must be 0..3660"); }
    }
    return {200, history(days)};
}
struct ApiServer::Impl {
    using Clock = std::chrono::steady_clock;
    struct Client { SOCKET socket; std::string request, response; size_t sent = 0; Clock::time_point started = Clock::now(); };
    SOCKET listener = INVALID_SOCKET;
    std::atomic<bool> stopping{false}; std::thread worker;
    std::string token; std::function<std::string(int)> history;
    bool winsock = false;
    Impl(int port, std::string key, std::function<std::string(int)> callback) : token(std::move(key)), history(std::move(callback)) {
        WSADATA data; if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("WSAStartup failed"); winsock = true;
        try {
            listener = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
            bool ipv6 = listener != INVALID_SOCKET;
            if (!ipv6) listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listener == INVALID_SOCKET) throw std::runtime_error("Cannot create API socket");
            BOOL exclusive = TRUE; if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char*)&exclusive, sizeof(exclusive))) throw std::runtime_error("Cannot reserve API port");
            int result;
            if (ipv6) {
                DWORD only = 0; if (setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&only, sizeof(only))) throw std::runtime_error("Cannot enable dual-stack API");
                sockaddr_in6 address{}; address.sin6_family = AF_INET6; address.sin6_port = htons((u_short)port); address.sin6_addr = in6addr_any;
                result = bind(listener, (sockaddr*)&address, sizeof(address));
            } else {
                sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons((u_short)port); address.sin_addr.s_addr = INADDR_ANY;
                result = bind(listener, (sockaddr*)&address, sizeof(address));
            }
            if (result || listen(listener, 16)) throw std::runtime_error("API port " + std::to_string(port) + " unavailable (Winsock " + std::to_string(WSAGetLastError()) + ")");
            u_long nonblock = 1; if (ioctlsocket(listener, FIONBIO, &nonblock)) throw std::runtime_error("Cannot configure API socket");
            worker = std::thread([this] { run(); });
        } catch (...) { if (listener != INVALID_SOCKET) closesocket(listener); WSACleanup(); winsock = false; throw; }
    }
    ~Impl() { stopping = true; if (worker.joinable()) worker.join(); if (listener != INVALID_SOCKET) closesocket(listener); if (winsock) WSACleanup(); }
    static std::string response(HttpResponse r) {
        return "HTTP/1.1 " + std::to_string(r.status) + " Result\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " + std::to_string(r.body.size()) + "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n" + r.body;
    }
    void run() noexcept {
        std::vector<Client> clients;
        try {
            while (!stopping) {
                fd_set readers, writers; FD_ZERO(&readers); FD_ZERO(&writers);
                if (clients.size() < 16) FD_SET(listener, &readers);
                for (const auto& c : clients) { if (c.response.empty()) FD_SET(c.socket, &readers); else FD_SET(c.socket, &writers); }
                timeval wait{0, 100000}; int ready = select(0, &readers, &writers, nullptr, &wait);
                if (ready == SOCKET_ERROR) throw std::runtime_error("API select failed");
                if (FD_ISSET(listener, &readers)) {
                    SOCKET s = accept(listener, nullptr, nullptr);
                    if (s != INVALID_SOCKET) { u_long n = 1; if (ioctlsocket(s, FIONBIO, &n)) closesocket(s); else clients.push_back(Client{s}); }
                }
                for (size_t i = 0; i < clients.size();) {
                    auto& c = clients[i]; bool close = Clock::now() - c.started > std::chrono::seconds(5);
                    if (!close && c.response.empty() && FD_ISSET(c.socket, &readers)) {
                        char data[2048]; int received = recv(c.socket, data, std::min<int>(sizeof(data), 8192 - (int)c.request.size()), 0);
                        if (received > 0) {
                            c.request.append(data, received);
                            size_t headerEnd = c.request.find("\r\n\r\n");
                            if (c.request.size() >= 8192 || headerEnd != std::string::npos) {
                                try { c.response = response(handleRequest(headerEnd == std::string::npos ? c.request : c.request.substr(0, headerEnd + 4), token, history)); }
                                catch (const std::exception& e) { logError(e.what()); c.response = response({500, "{\"error\":\"History unavailable\"}"}); }
                            }
                        } else if (received == 0 || WSAGetLastError() != WSAEWOULDBLOCK) close = true;
                    }
                    if (!close && !c.response.empty() && FD_ISSET(c.socket, &writers)) {
                        int sent = send(c.socket, c.response.data() + c.sent, (int)std::min<size_t>(c.response.size() - c.sent, 65536), 0);
                        if (sent > 0) { c.sent += sent; if (c.sent == c.response.size()) close = true; }
                        else if (sent == 0 || WSAGetLastError() != WSAEWOULDBLOCK) close = true;
                    }
                    if (close) { closesocket(c.socket); clients.erase(clients.begin() + i); } else ++i;
                }
            }
        } catch (const std::exception& e) { logError(e.what()); }
        for (const auto& c : clients) closesocket(c.socket);
    }
};
ApiServer::ApiServer(int port, const std::string& token, std::function<std::string(int)> callback) : impl(std::make_unique<Impl>(port, token, std::move(callback))) { }
ApiServer::~ApiServer() = default;
}
