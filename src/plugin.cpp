#define MUMBLE_PLUGIN_NO_DEFAULT_FUNCTION_DEFINITIONS
#include "MumblePlugin.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
extern "C" IMAGE_DOS_HEADER __ImageBase;
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "audio_state.h"
#include "config.h"
#include "connection.h"

namespace {
#if defined(THEISLE_SPATIAL_STEAMDECK)
constexpr const char* kPluginBaseName = "theisle_spatial_steamdeck";
constexpr const char* kPluginDisplayName = "Bosch Island - Spatial Audio (Steam Deck)";
#else
constexpr const char* kPluginBaseName = "theisle_spatial";
constexpr const char* kPluginDisplayName = "Bosch Island - Spatial Audio";
#endif

std::filesystem::path resolveModuleDir() {
#ifdef _WIN32
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA((HMODULE)&__ImageBase, path, MAX_PATH) == 0) {
        return ".";
    }
    return std::filesystem::path(path).parent_path();
#else
    Dl_info info{};
    if (dladdr((void*)&resolveModuleDir, &info) == 0 || info.dli_fname == nullptr) {
        return ".";
    }
    return std::filesystem::path(info.dli_fname).parent_path();
#endif
}

std::string getEnvVar(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return "";
    }
    return value;
}

uint64_t tickMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string pluginIniFileName() {
    return std::string(kPluginBaseName) + ".ini";
}

std::string pluginLogFileName() {
    return std::string(kPluginBaseName) + ".log";
}
}

static mumble_plugin_id_t  g_pluginID   = 0;
static MumbleAPI           g_api        = {};
static mumble_connection_t g_conn       = 0;
static bool                g_synced     = false;
static Config              g_cfg;
static std::string         g_localHash;

static AudioState g_audio;

static std::mutex g_hashMtx;
static std::unordered_map<mumble_userid_t, std::string> g_hashMap;

static std::unique_ptr<FalloffConnection> g_conn_obj;

static struct AudioDiag {
    std::mutex   mtx;
    uint32_t     gainsApplied = 0;
    float        gainMin      = 2.0f;
    float        gainMax      = -1.0f;
    uint64_t     lastLogMs    = 0;
} g_diag;

static std::atomic<bool>          g_attenLoggedOnce{false};
static std::atomic<uint32_t>      g_unknownSrcCount{0};       // sources seen with no hashMap entry
static std::atomic<uint32_t>      g_staleMappingsCleared{0};  // g_audio entries wiped at HELLO OK
static std::atomic<uint32_t>      g_remapCount{0};            // refreshHashMap() calls from lifecycle events
static std::atomic<uint32_t>      g_staleEvictions{0};        // g_audio entries removed on user-leave
static std::atomic<uint64_t>       g_lastUnknownRefreshMs{0};
static std::unordered_set<std::string> g_mappingLogged;


static void pluginLog(const char* msg) {
    if (g_pluginID && g_cfg.debugLog)
        g_api.log(g_pluginID, msg);
}

static void refreshHashMap() {
    if (!g_synced || !g_pluginID) return;

    mumble_userid_t* users    = nullptr;
    size_t           count    = 0;
    if (g_api.getAllUsers(g_pluginID, g_conn, &users, &count) != MUMBLE_STATUS_OK)
        return;

    std::unordered_map<mumble_userid_t, std::string> newMap;
    newMap.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const char* hash = nullptr;
        if (g_api.getUserHash(g_pluginID, g_conn, users[i], &hash) == MUMBLE_STATUS_OK && hash) {
            newMap[users[i]] = hash;
            g_api.freeMemory(g_pluginID, hash);
        }
    }
    g_api.freeMemory(g_pluginID, users);

    {
        std::lock_guard<std::mutex> lk(g_hashMtx);
        g_hashMap = std::move(newMap);
    }
}

static void addUserHash(mumble_userid_t uid) {
    if (!g_pluginID || !g_synced) return;
    const char* hash = nullptr;
    if (g_api.getUserHash(g_pluginID, g_conn, uid, &hash) == MUMBLE_STATUS_OK && hash) {
        std::lock_guard<std::mutex> lk(g_hashMtx);
        g_hashMap[uid] = hash;
        g_api.freeMemory(g_pluginID, hash);
    }
}

static void removeUserHash(mumble_userid_t uid) {
    std::lock_guard<std::mutex> lk(g_hashMtx);
    g_hashMap.erase(uid);
}

static void startConnection() {
    if (!g_cfg.enabled || g_localHash.empty()) return;
    if (g_conn_obj && g_conn_obj->isConnected()) return;

    g_conn_obj = std::make_unique<FalloffConnection>(
        g_cfg,
        g_localHash,
        [](const std::string& uid, float gain, float pan) {
            g_audio.update(uid, gain, pan);
            std::lock_guard<std::mutex> lk(g_diag.mtx);
            g_diag.gainsApplied++;
            if (gain < g_diag.gainMin) g_diag.gainMin = gain;
            if (gain > g_diag.gainMax) g_diag.gainMax = gain;
        },
        []() {
            g_audio.reset();
        },
        // HelloOkCb: fired on connection thread immediately after HELLO auth.
        // Hard-reset stale audio state so previous-session gain/pan values
        // never bleed into this session.  Then rebuild the userId->hash map
        // to recover any user-add events that were missed or failed silently.
        []() {
            size_t cleared = g_audio.resetWithCount();
            g_staleMappingsCleared.store((uint32_t)cleared, std::memory_order_relaxed);
            refreshHashMap();
            char helloLog[128];
            snprintf(helloLog, sizeof(helloLog),
                "[theisle_spatial] hello_ok_reset stale_cleared=%zu map_size=%zu",
                cleared, g_audio.size());
            FalloffConnection::logToFile(helloLog);
            if (g_cfg.debugLog) pluginLog(helloLog);
        },
        [](const std::string& msg) {
            pluginLog((std::string("[theisle_spatial] ") + msg).c_str());
        }
    );
    g_conn_obj->start();
    pluginLog("[theisle_spatial] falloff connection started");
}

static void stopConnection() {
    if (g_conn_obj) {
        g_conn_obj->stop();
        g_conn_obj.reset();
    }
    g_audio.reset();
    g_attenLoggedOnce.store(false);
    {
        std::lock_guard<std::mutex> lk(g_hashMtx);
        g_mappingLogged.clear();
    }
}


MUMBLE_PLUGIN_EXPORT mumble_error_t MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_init(mumble_plugin_id_t id) {
    g_pluginID = id;

    const std::filesystem::path moduleDir = resolveModuleDir();
    const std::string iniPath = (moduleDir / pluginIniFileName()).string();
    g_cfg = Config::load(iniPath);
    g_audio.setAlpha(g_cfg.smoothAlpha);
    g_audio.setPanAlpha(g_cfg.panSmoothAlpha);

    std::string hostOverride = getEnvVar("THEISLE_SPATIAL_HOST");
    if (!hostOverride.empty()) {
        g_cfg.serverHost = hostOverride;
    }

    std::string portOverride = getEnvVar("THEISLE_SPATIAL_PORT");
    if (!portOverride.empty()) {
        try { g_cfg.serverPort = std::stoi(portOverride); } catch (...) {}
    }

    const std::string logPath = (moduleDir / pluginLogFileName()).string();
    FalloffConnection::openLogFile(logPath);

    // Startup report â€” always emitted regardless of debug_log
    auto initLog = [&](const std::string& msg) {
        if (g_pluginID) g_api.log(g_pluginID, msg.c_str());
        FalloffConnection::logToFile(msg);
    };

    initLog(std::string("[theisle_spatial] ini_path=") + g_cfg.loadedFrom
            + " found=" + (g_cfg.fileFound ? "1" : "0"));
    {
        char report[512];
        snprintf(report, sizeof(report),
            "[theisle_spatial] config active_server=%s selected_server=%s host=%s port=%d enabled=%d debug_log=%d"
            " smoothing=%.2f pan_smoothing=%.2f",
            g_cfg.activeServer.empty() ? "legacy" : g_cfg.activeServer.c_str(),
            g_cfg.selectedServer.c_str(),
            g_cfg.serverHost.c_str(), g_cfg.serverPort,
            (int)g_cfg.enabled, (int)g_cfg.debugLog,
            g_cfg.smoothAlpha, g_cfg.panSmoothAlpha);
        initLog(report);
    }

    // Close log file again if debug mode is off (init messages already flushed)
    if (!g_cfg.debugLog) {
        FalloffConnection::openLogFile("");
    }

    return MUMBLE_STATUS_OK;
}

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_shutdown() {
    stopConnection();
    g_synced = false;
    g_localHash.clear();
    {
        std::lock_guard<std::mutex> lk(g_hashMtx);
        g_hashMap.clear();
    }
}

MUMBLE_PLUGIN_EXPORT MumbleStringWrapper MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_getName() {
    return { kPluginDisplayName, std::strlen(kPluginDisplayName), false };
}

MUMBLE_PLUGIN_EXPORT mumble_version_t MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_getAPIVersion() {
    return { MUMBLE_PLUGIN_API_MAJOR_MACRO, MUMBLE_PLUGIN_API_MINOR_MACRO, MUMBLE_PLUGIN_API_PATCH_MACRO };
}

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_registerAPIFunctions(void* apiStruct) {
    g_api = MUMBLE_API_CAST(apiStruct);
}

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_releaseResource(const void* ptr) {
    delete[] static_cast<const char*>(ptr);
}


MUMBLE_PLUGIN_EXPORT mumble_version_t MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_getVersion() {
    return { 1, 0, 1 };
}

MUMBLE_PLUGIN_EXPORT MumbleStringWrapper MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_getAuthor() {
    static const char author[] = "GoryHoles";
    return { author, sizeof(author) - 1, false };
}

MUMBLE_PLUGIN_EXPORT MumbleStringWrapper MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_getDescription() {
    static const char desc[] =
        "Optional spatial audio enhancement for The Isle. "
        "Connects to the falloff server and applies per-speaker gain and "
        "panning based on in-game positions. Baseline channel proximity "
        "continues to function for users without this plugin.";
    return { desc, sizeof(desc) - 1, false };
}

MUMBLE_PLUGIN_EXPORT uint32_t MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_getFeatures() {
    return MUMBLE_FEATURE_AUDIO;
}


MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_onServerConnected(mumble_connection_t connection) {
    g_conn = connection;
}

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_onServerDisconnected(mumble_connection_t /*connection*/) {
    stopConnection();
    g_synced = false;
    g_localHash.clear();
    {
        std::lock_guard<std::mutex> lk(g_hashMtx);
        g_hashMap.clear();
    }
}

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_onServerSynchronized(mumble_connection_t connection) {
    g_conn   = connection;
    g_synced = true;

    mumble_userid_t localID = 0;
    if (g_api.getLocalUserID(g_pluginID, connection, &localID) == MUMBLE_STATUS_OK) {
        const char* hash = nullptr;
        if (g_api.getUserHash(g_pluginID, connection, localID, &hash) == MUMBLE_STATUS_OK && hash) {
            g_localHash = hash;
            g_api.freeMemory(g_pluginID, hash);
        }
    }

    if (g_cfg.debugLog) {
        std::string msg = "[theisle_spatial] synced, local_hash=" + g_localHash;
        pluginLog(msg.c_str());
    }

    refreshHashMap();
    startConnection();

    // One-shot diagnostic: log local SHA-1 and first 3 remote SHA-1 hashes
    {
        std::string uidLog = "[theisle_spatial] uid_canonical local_sha1="
                           + (g_localHash.size() >= 12 ? g_localHash.substr(0, 12) : g_localHash)
                           + "...";
        std::lock_guard<std::mutex> lk(g_hashMtx);
        int n = 0;
        for (auto& kv : g_hashMap) {
            if (n >= 3) break;
            uidLog += " remote" + std::to_string(n) + "_sha1="
                    + (kv.second.size() >= 12 ? kv.second.substr(0, 12) : kv.second)
                    + "...";
            ++n;
        }
        FalloffConnection::logToFile(uidLog);
        if (g_cfg.debugLog) pluginLog(uidLog.c_str());
    }
}


MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_onUserAdded(mumble_connection_t /*connection*/, mumble_userid_t /*userID*/) {
    // Full rebuild instead of single-entry add.  addUserHash() can silently fail if
    // Mumble hasn't fully initialised the joining user yet; getAllUsers covers everyone.
    refreshHashMap();
    g_remapCount.fetch_add(1, std::memory_order_relaxed);
}

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_onUserRemoved(mumble_connection_t /*connection*/, mumble_userid_t userID) {
    // Evict the departing user's gain/pan state so stale values cannot be
    // re-applied if a new user is later assigned the same session ID or the
    // same user rejoins before the next HELLO OK rebuild.
    std::string evictHash;
    {
        std::lock_guard<std::mutex> lk(g_hashMtx);
        auto it = g_hashMap.find(userID);
        if (it != g_hashMap.end()) {
            evictHash = it->second;
            g_hashMap.erase(it);
        }
    }
    if (!evictHash.empty() && g_audio.evict(evictHash))
        g_staleEvictions.fetch_add(1, std::memory_order_relaxed);
    g_remapCount.fetch_add(1, std::memory_order_relaxed);
}


// Channel-move events: a user changing channel does not alter their session ID
// so g_hashMap stays valid, but the proximity group may change and we want the
// mapping confirmed fresh before applying audio transforms to moved speakers.
MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_onChannelEntered(mumble_connection_t /*connection*/,
                        mumble_userid_t     /*userID*/,
                        mumble_channelid_t  /*previousChannelID*/,
                        mumble_channelid_t  /*newChannelID*/) {
    refreshHashMap();
    g_remapCount.fetch_add(1, std::memory_order_relaxed);
}

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_onChannelExited(mumble_connection_t /*connection*/,
                       mumble_userid_t     /*userID*/,
                       mumble_channelid_t  /*channelID*/) {
    refreshHashMap();
    g_remapCount.fetch_add(1, std::memory_order_relaxed);
}

MUMBLE_PLUGIN_EXPORT bool MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_onAudioSourceFetched(float*   outputPCM,
                             uint32_t sampleCount,
                             uint16_t channelCount,
                             uint32_t /*sampleRate*/,
                             bool     isSpeech,
                             mumble_userid_t userID) {
    if (!isSpeech || !g_cfg.enabled) return false;

    std::string hash;
    {
        std::lock_guard<std::mutex> lk(g_hashMtx);
        auto it = g_hashMap.find(userID);
        if (it == g_hashMap.end()) {
            g_unknownSrcCount.fetch_add(1, std::memory_order_relaxed);
            // Throttled self-heal: if the hash map has been stale for >1s, rebuild it.
            // This recovers from missed user-add events without spamming getAllUsers.
            uint64_t nowMs = tickMs();
            uint64_t lastMs = g_lastUnknownRefreshMs.load(std::memory_order_relaxed);
            if (nowMs - lastMs >= 1000) {
                uint64_t expected = lastMs;
                if (g_lastUnknownRefreshMs.compare_exchange_strong(
                        expected, nowMs,
                        std::memory_order_relaxed, std::memory_order_relaxed)) {
                    refreshHashMap();
                    g_remapCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
            return false;
        }
        hash = it->second;
    }

    // Once-per-uid: log hash lookup so operator can compare with UPDATE payload uids
    {
        std::lock_guard<std::mutex> lk(g_hashMtx);
        if (g_mappingLogged.find(hash) == g_mappingLogged.end()) {
            g_mappingLogged.insert(hash);
            char mapLog[160];
            snprintf(mapLog, sizeof(mapLog),
                "[theisle_spatial] uid_check mumble_id=%u hash=%.12s... audio_state_has=%d",
                (unsigned)userID, hash.c_str(), (int)g_audio.has(hash));
            pluginLog(mapLog);
        }
    }

    bool hasEntry = g_audio.has(hash);

    // Once-log: attenuation active vs bypassed
    if (!g_attenLoggedOnce.exchange(true)) {
        char atLog[128];
        snprintf(atLog, sizeof(atLog),
            "[theisle_spatial] atten_check uid=%u hash=%.12s... active=%d",
            (unsigned)userID, hash.c_str(), (int)hasEntry);
        pluginLog(atLog);
    }

    // 5s periodic client stats
    {
        uint64_t nowMs = tickMs();
        std::lock_guard<std::mutex> lk(g_diag.mtx);
        if (nowMs - g_diag.lastLogMs >= 5000) {
            uint32_t frames   = g_conn_obj ? g_conn_obj->drainFrameCount() : 0;
            uint32_t rxLines  = g_conn_obj ? g_conn_obj->drainLineCount()  : 0;
            size_t mappedSrcs   = g_audio.size();
            uint32_t unkSrc     = g_unknownSrcCount.exchange(0, std::memory_order_relaxed);
            uint32_t staleClr   = g_staleMappingsCleared.exchange(0, std::memory_order_relaxed);
            uint32_t remapCnt   = g_remapCount.exchange(0, std::memory_order_relaxed);
            uint32_t evictions  = g_staleEvictions.exchange(0, std::memory_order_relaxed);
            size_t hashMapSz;
            { std::lock_guard<std::mutex> lk(g_hashMtx); hashMapSz = g_hashMap.size(); }
            char statsLog[384];
            snprintf(statsLog, sizeof(statsLog),
                "[theisle_spatial] audio_stats rx_lines=%u update_pkts=%u gains_applied=%u "
                "min=%.3f max=%.3f mapped_srcs=%zu hash_entries=%zu "
                "stale_cleared=%u unknown_src=%u remap_count=%u stale_evictions=%u"
                " gain_a=%.2f pan_a=%.2f",
                rxLines, frames, g_diag.gainsApplied,
                g_diag.gainsApplied > 0 ? g_diag.gainMin : 0.0f,
                g_diag.gainsApplied > 0 ? g_diag.gainMax : 0.0f,
                mappedSrcs, hashMapSz, staleClr, unkSrc, remapCnt, evictions,
                g_cfg.smoothAlpha, g_cfg.panSmoothAlpha);
            pluginLog(statsLog);
            g_diag.gainsApplied = 0;
            g_diag.gainMin      = 2.0f;
            g_diag.gainMax      = -1.0f;
            g_diag.lastLogMs    = nowMs;
        }
    }

    if (!hasEntry) {
        return false;
    }

    AudioState::Entry e = g_audio.get(hash);

    // Clamp to valid range â€” gain must be < 1.0 at distance for attenuation to be visible
    float gain = std::max(0.0f, std::min(1.0f, e.gain));
    float pan  = std::max(-1.0f, std::min(1.0f, e.pan));

    if (channelCount == 2) {
        float leftGain  = gain * std::sqrt(0.5f * (1.0f - pan));
        float rightGain = gain * std::sqrt(0.5f * (1.0f + pan));
        for (uint32_t i = 0; i < sampleCount; ++i) {
            outputPCM[i * 2 + 0] *= leftGain;
            outputPCM[i * 2 + 1] *= rightGain;
        }
    } else {
        for (uint32_t i = 0; i < sampleCount * channelCount; ++i)
            outputPCM[i] *= gain;
    }

    return true;
}


MUMBLE_PLUGIN_EXPORT bool MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_onAudioInput(short* /*inputPCM*/, uint32_t /*sampleCount*/,
                    uint16_t /*channelCount*/, uint32_t /*sampleRate*/,
                    bool /*isSpeech*/) {
    return false;
}

MUMBLE_PLUGIN_EXPORT bool MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_onAudioOutputAboutToPlay(float* /*outputPCM*/, uint32_t /*sampleCount*/,
                                uint16_t /*channelCount*/, uint32_t /*sampleRate*/) {
    return false;
}


MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_onDeactivated(uint32_t features) {
    if (features & MUMBLE_FEATURE_AUDIO) {
        g_audio.reset();
        pluginLog("[theisle_spatial] audio feature deactivated - gains reset");
    }
}
