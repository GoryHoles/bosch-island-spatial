#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
static constexpr int SOCKET_ERROR = -1;
#endif

#include "connection.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

static std::string trimStr(const std::string& s) {
    const char* ws = " \t\r\n";
    size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

static FILE* g_logFile = nullptr;
static std::mutex g_logMtx;

static int lastSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static void initializeSockets() {
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

static void closeSocketHandle(socket_t socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

static bool isRetryableSendError(int error) {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
    return error == EWOULDBLOCK || error == EAGAIN || error == EINTR;
#endif
}

static bool isRecvTimeoutError(int error) {
#ifdef _WIN32
    return error == WSAETIMEDOUT;
#else
    return error == EWOULDBLOCK || error == EAGAIN;
#endif
}

static bool isInterruptedError(int error) {
#ifdef _WIN32
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}

static void setSocketTimeout(socket_t socket, int option, int timeoutMs) {
#ifdef _WIN32
    DWORD timeout = (DWORD)timeoutMs;
    setsockopt(socket, SOL_SOCKET, option, (const char*)&timeout, sizeof(timeout));
#else
    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(socket, SOL_SOCKET, option, &timeout, sizeof(timeout));
#endif
}

static uint64_t tickMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void FalloffConnection::logToFile(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    if (g_logFile) {
        fprintf(g_logFile, "%s\n", msg.c_str());
        fflush(g_logFile);
    }
}

void FalloffConnection::openLogFile(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    if (!path.empty()) {
        g_logFile = fopen(path.c_str(), "a");
    }
}

static void logMsg(const Config& cfg, const FalloffConnection::LogCb& cb, const std::string& msg) {
    if (!cfg.debugLog) return;
    if (cb) cb(msg);
    std::lock_guard<std::mutex> lk(g_logMtx);
    if (g_logFile) {
        fprintf(g_logFile, "%s\n", msg.c_str());
        fflush(g_logFile);
    }
}

static int sendAll(socket_t socket, const char* data, int len) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::seconds(5);
    int sent = 0;
    while (sent < len) {
        int n = send(socket, data + sent, len - sent, 0);
        if (n == SOCKET_ERROR) {
            const int error = lastSocketError();
            if (isRetryableSendError(error)) {
                if (clock::now() >= deadline) return sent;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            return -1;
        }
        if (n == 0) return sent;
        sent += n;
    }
    return sent;
}

FalloffConnection::FalloffConnection(Config cfg, std::string localUid,
                                     UpdateCb onUpdate, DisconnectCb onDisconnect,
                                     HelloOkCb onHelloOk, LogCb onLog)
    : m_cfg(std::move(cfg)), m_uid(std::move(localUid)),
      m_onUpdate(std::move(onUpdate)), m_onDisconnect(std::move(onDisconnect)),
      m_onHelloOk(std::move(onHelloOk)), m_onLog(std::move(onLog)) {}

FalloffConnection::~FalloffConnection() { stop(); }

void FalloffConnection::start() {
    m_stop = false;
    m_thread = std::thread(&FalloffConnection::run, this);
}

void FalloffConnection::stop() {
    m_stop = true;
    closeSocket();
    if (m_thread.joinable()) m_thread.join();
    m_connected = false;
}

void FalloffConnection::closeSocket() {
    socket_t socket = (socket_t)m_sock;
    if (socket == kInvalidSocket) return;
    closeSocketHandle(socket);
    m_sock = (uintptr_t)kInvalidSocket;
}

static std::string resolveIP(const std::string& host, int preferFamily) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = preferFamily;

    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) return "";

    char buffer[INET6_ADDRSTRLEN] = {};
    if (result->ai_family == AF_INET) {
        inet_ntop(AF_INET, &((sockaddr_in*)result->ai_addr)->sin_addr, buffer, sizeof(buffer));
    } else if (result->ai_family == AF_INET6) {
        inet_ntop(AF_INET6, &((sockaddr_in6*)result->ai_addr)->sin6_addr, buffer, sizeof(buffer));
    }

    freeaddrinfo(result);
    return buffer;
}

bool FalloffConnection::tryConnect() {
    if (m_cfg.apiKey.empty()) {
        logMsg(m_cfg, m_onLog, "falloff_connect_skip reason=empty_api_key");
        return false;
    }
    if (m_uid.empty()) {
        logMsg(m_cfg, m_onLog, "falloff_connect_skip reason=empty_uid");
        return false;
    }

    initializeSockets();

    std::string resolvedIP = resolveIP(m_cfg.serverHost, AF_INET);
    int resolvedFamily = AF_INET;
    if (resolvedIP.empty()) {
        resolvedIP = resolveIP(m_cfg.serverHost, AF_UNSPEC);
        resolvedFamily = AF_UNSPEC;
    }

    {
        char diag[256];
        snprintf(diag, sizeof(diag),
            "falloff_connect_attempt host=%s port=%d resolved_ip=%s family=%s",
            m_cfg.serverHost.c_str(), m_cfg.serverPort,
            resolvedIP.empty() ? "none" : resolvedIP.c_str(),
            resolvedFamily == AF_INET ? "AF_INET" : "AF_UNSPEC");
        logMsg(m_cfg, m_onLog, diag);
    }

    if (resolvedIP.empty()) {
        char diag[128];
        snprintf(diag, sizeof(diag),
            "falloff_connect_fail reason=resolve_failed host=%s error=%d",
            m_cfg.serverHost.c_str(), lastSocketError());
        logMsg(m_cfg, m_onLog, diag);
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)m_cfg.serverPort);
    if (inet_pton(AF_INET, resolvedIP.c_str(), &addr.sin_addr) != 1) {
        char diag[128];
        snprintf(diag, sizeof(diag),
            "falloff_connect_fail reason=inet_pton_failed resolved_ip=%s",
            resolvedIP.c_str());
        logMsg(m_cfg, m_onLog, diag);
        return false;
    }

    socket_t socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kInvalidSocket) {
        char diag[64];
        snprintf(diag, sizeof(diag), "falloff_connect_fail reason=socket_create error=%d", lastSocketError());
        logMsg(m_cfg, m_onLog, diag);
        return false;
    }

    setSocketTimeout(socket, SO_SNDTIMEO, 5000);
    setSocketTimeout(socket, SO_RCVTIMEO, 30000);

    if (connect(socket, (sockaddr*)&addr, sizeof(addr)) != 0) {
        char diag[192];
        snprintf(diag, sizeof(diag),
            "falloff_connect_fail reason=connect host=%s port=%d resolved_ip=%s socket_family=AF_INET error=%d",
            m_cfg.serverHost.c_str(), m_cfg.serverPort, resolvedIP.c_str(), lastSocketError());
        logMsg(m_cfg, m_onLog, diag);
        closeSocketHandle(socket);
        return false;
    }

    std::string caps = "{\"version\":\"1.0\",\"features\":[\"spatial\"]}";
    std::string hello = "HELLO " + m_cfg.apiKey + " " + m_uid + " " + caps + "\n";

    {
        char diag[256];
        snprintf(diag, sizeof(diag),
            "falloff_hello_prepare api_key_present=1 uid_present=1 caps_len=%d hello_len=%d",
            (int)caps.size(), (int)hello.size());
        logMsg(m_cfg, m_onLog, diag);
    }

    int sent = sendAll(socket, hello.c_str(), (int)hello.size());
    if (sent != (int)hello.size()) {
        char diag[128];
        snprintf(diag, sizeof(diag),
            "falloff_connect_fail reason=send_incomplete sent=%d expected=%d",
            sent, (int)hello.size());
        logMsg(m_cfg, m_onLog, diag);
        closeSocketHandle(socket);
        return false;
    }
    logMsg(m_cfg, m_onLog, "falloff_hello_sent bytes_ok=1");

    char buffer[256] = {};
    int received = recv(socket, buffer, (int)sizeof(buffer) - 1, 0);
    if (received == 0) {
        logMsg(m_cfg, m_onLog, "falloff_hello_reply close_reason=server_closed");
        closeSocketHandle(socket);
        return false;
    }
    if (received < 0) {
        char diag[80];
        snprintf(diag, sizeof(diag),
            "falloff_hello_reply close_reason=recv_error error=%d", lastSocketError());
        logMsg(m_cfg, m_onLog, diag);
        closeSocketHandle(socket);
        return false;
    }

    buffer[received] = '\0';
    std::string reply(buffer);
    if (reply.find("OK") != std::string::npos) {
        logMsg(m_cfg, m_onLog, "falloff_hello_reply ok");
    } else if (reply.find("ERR") != std::string::npos) {
        logMsg(m_cfg, m_onLog, "falloff_hello_reply unauthorized");
        closeSocketHandle(socket);
        return false;
    } else {
        logMsg(m_cfg, m_onLog, "falloff_hello_reply malformed_reply");
        closeSocketHandle(socket);
        return false;
    }

    setSocketTimeout(socket, SO_RCVTIMEO, 10000);

    m_sock = (uintptr_t)socket;
    m_connected = true;
    return true;
}

void FalloffConnection::parseLine(const std::string& line) {
    if (line.size() < 4) return;

    size_t sp = line.find(' ');
    if (sp == std::string::npos) return;

    std::string cmd = line.substr(0, sp);
    std::string rest = trimStr(line.substr(sp + 1));
    bool hasPan = cmd == "UPDATE2";

    if (cmd != "UPDATE" && cmd != "UPDATE2") return;
    if (rest.empty()) return;

    struct Entry {
        std::string uid;
        float gain;
        float pan;
    };

    std::vector<Entry> entries;
    std::istringstream stream(rest);
    std::string token;
    while (std::getline(stream, token, ',')) {
        token = trimStr(token);
        if (token.empty()) continue;

        std::istringstream tokenStream(token);
        std::string uid;
        float gain = 1.0f;
        float pan = 0.0f;

        if (!(tokenStream >> uid >> gain)) continue;
        if (hasPan) tokenStream >> pan;

        gain = std::max(0.0f, std::min(1.0f, gain));
        pan = std::max(-1.0f, std::min(1.0f, pan));
        entries.push_back({uid, gain, pan});
    }

    if (entries.empty()) return;

    m_frameCount.fetch_add(1, std::memory_order_relaxed);

    if (m_cfg.debugLog) {
        std::string uidLog = "update_uids cmd=" + cmd + " entries=" + std::to_string(entries.size());
        for (size_t i = 0; i < entries.size() && i < 3; ++i) {
            const auto& entry = entries[i];
            char buffer[64];
            snprintf(buffer, sizeof(buffer), " uid%zu=%.8s gain=%.3f", i, entry.uid.c_str(), entry.gain);
            uidLog += buffer;
        }
        logMsg(m_cfg, m_onLog, uidLog);
    }

    for (const auto& entry : entries) {
        m_onUpdate(entry.uid, entry.gain, entry.pan);
    }
}

void FalloffConnection::run() {
    int backoffSec = 1;

    while (!m_stop.load()) {
        if (!tryConnect()) {
            for (int i = 0; i < backoffSec * 10 && !m_stop.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            backoffSec = std::min(backoffSec * 2, 60);
            continue;
        }

        backoffSec = 1;
        if (m_onHelloOk) m_onHelloOk();

        socket_t socket = (socket_t)m_sock;
        std::string buffer;
        char chunk[2048];
        static const char kPing[] = "PING\n";

        uint64_t connectedMs = tickMs();
        uint64_t lastRxMs = connectedMs;
        uint64_t lastTxMs = connectedMs;
        uint64_t lastAliveMs = connectedMs;

        while (!m_stop.load()) {
            int received = recv(socket, chunk, (int)sizeof(chunk) - 1, 0);

            if (received > 0) {
                lastRxMs = tickMs();
                chunk[received] = '\0';
                buffer += chunk;

                size_t pos = 0;
                while ((pos = buffer.find('\n')) != std::string::npos) {
                    std::string line = trimStr(buffer.substr(0, pos));
                    buffer = buffer.substr(pos + 1);
                    m_rxLineCount.fetch_add(1, std::memory_order_relaxed);
                    if (line == "PONG") continue;
                    if (!line.empty()) parseLine(line);
                }
                continue;
            }

            if (received == 0) {
                logMsg(m_cfg, m_onLog, "falloff_disconnect close_reason=server_eof");
                break;
            }

            const int error = lastSocketError();
            if (m_stop.load()) {
                FalloffConnection::logToFile("falloff_disconnect close_reason=shutdown");
                break;
            }

            if (isRecvTimeoutError(error)) {
                uint64_t nowMs = tickMs();
                if (sendAll(socket, kPing, (int)(sizeof(kPing) - 1)) < 0) {
                    FalloffConnection::logToFile("falloff_disconnect close_reason=ping_send_failed");
                    break;
                }
                lastTxMs = nowMs;
                if (nowMs - lastAliveMs >= 30000) {
                    char alive[128];
                    snprintf(alive, sizeof(alive),
                        "falloff_alive connected=1 last_rx=%.0fs last_tx=%.0fs",
                        (nowMs - lastRxMs) / 1000.0,
                        (nowMs - lastTxMs) / 1000.0);
                    logMsg(m_cfg, m_onLog, alive);
                    lastAliveMs = nowMs;
                }
                continue;
            }

            if (isInterruptedError(error)) continue;

            char diag[80];
            snprintf(diag, sizeof(diag), "falloff_disconnect close_reason=fatal_socket error=%d", error);
            logMsg(m_cfg, m_onLog, diag);
            break;
        }

        closeSocket();
        m_connected = false;
        if (m_onDisconnect) m_onDisconnect();
        if (m_stop.load()) {
            FalloffConnection::logToFile("falloff_close close_reason=shutdown");
            break;
        }

        for (int i = 0; i < backoffSec * 10 && !m_stop.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        backoffSec = std::min(backoffSec * 2, 60);
    }

    m_connected = false;
}
