#pragma once
#include <unordered_map>
#include <mutex>
#include <string>
#include <cmath>
#include <algorithm>

class AudioState {
public:
    struct Entry {
        float gain = 1.0f;
        float pan  = 0.0f;
        bool  active = false;
    };

    void update(const std::string& uid, float gain, float pan) {
        gain = std::max(0.0f, std::min(1.0f, gain));
        pan  = std::max(-1.0f, std::min(1.0f, pan));
        std::lock_guard<std::mutex> lk(m_mtx);
        auto& e = m_entries[uid];
        e.gain   = m_alpha    * gain + (1.0f - m_alpha)    * e.gain;
        e.pan    = m_panAlpha * pan  + (1.0f - m_panAlpha) * e.pan;
        e.active = true;
    }

    Entry get(const std::string& uid) const {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_entries.find(uid);
        return (it != m_entries.end()) ? it->second : Entry{};
    }

    bool has(const std::string& uid) const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_entries.count(uid) > 0;
    }

    void reset() {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_entries.clear();
    }

    bool evict(const std::string& uid) {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_entries.erase(uid) > 0;
    }

    size_t resetWithCount() {
        std::lock_guard<std::mutex> lk(m_mtx);
        size_t n = m_entries.size();
        m_entries.clear();
        return n;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_entries.size();
    }

    void setAlpha(float a) {
        m_alpha = std::max(0.01f, std::min(1.0f, a));
    }

    void setPanAlpha(float a) {
        m_panAlpha = std::max(0.01f, std::min(1.0f, a));
    }

private:
    mutable std::mutex m_mtx;
    std::unordered_map<std::string, Entry> m_entries;
    float m_alpha    = 0.80f;  // gain EMA: near-instant (server already pre-smoothed at 0.45)
    float m_panAlpha = 0.95f;  // pan EMA: near-passthrough for realtime bearing
};
