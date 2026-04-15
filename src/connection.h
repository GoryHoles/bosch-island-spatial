#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include "config.h"

class FalloffConnection {
public:
    using UpdateCb     = std::function<void(const std::string& uid, float gain, float pan)>;
    using DisconnectCb = std::function<void()>;
    // Fired once on the connection thread immediately after HELLO auth succeeds.
    // Resets stale audio-state and rebuilds the source->hash map so entries
    // from a previous session are never mis-applied to a different speaker.
    using HelloOkCb    = std::function<void()>;
    using LogCb = std::function<void(const std::string& msg)>;

    FalloffConnection(Config cfg,
                      std::string localUid,
                      UpdateCb onUpdate,
                      DisconnectCb onDisconnect,
                      HelloOkCb onHelloOk,
                      LogCb onLog);
    ~FalloffConnection();

    void     start();
    void     stop();
    bool     isConnected()    const { return m_connected.load(); }
    uint32_t drainFrameCount()     { return m_frameCount.exchange(0, std::memory_order_relaxed); }
    uint32_t drainLineCount()      { return m_rxLineCount.exchange(0, std::memory_order_relaxed); }

    static void openLogFile(const std::string& path);
    static void logToFile(const std::string& msg);

private:
    void   run();
    bool   tryConnect();
    void   parseLine(const std::string& line);
    void   closeSocket();

    Config      m_cfg;
    std::string m_uid;
    UpdateCb    m_onUpdate;
    DisconnectCb m_onDisconnect;
    HelloOkCb    m_onHelloOk;
    LogCb m_onLog;

    std::thread       m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_connected{false};

    uintptr_t m_sock = (uintptr_t)(~0ULL);

    std::atomic<uint32_t> m_frameCount{0};
    std::atomic<uint32_t> m_rxLineCount{0};
};
