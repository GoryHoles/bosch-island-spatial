#pragma once
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

struct Config {
    struct ServerEndpoint {
        std::string host;
        int         port = 0;
        bool        hasHost = false;
        bool        hasPort = false;

        bool isValid() const {
            return hasHost && hasPort && !host.empty() && port > 0 && port <= 65535;
        }
    };

    std::string serverHost  = "37.187.144.114";
    int         serverPort  = 8890;
    std::string activeServer;
    std::string selectedServer = "legacy";
    std::string apiKey      = "change_me";
    bool        enabled     = true;
    bool        debugLog    = false;
    float       smoothAlpha    = 0.80f;
    float       panSmoothAlpha = 0.95f;
    std::string loadedFrom;
    bool        fileFound   = false;

    static Config load(const std::string& iniPath) {
        Config cfg;
        cfg.loadedFrom = iniPath;

        std::ifstream f(iniPath);
        if (!f.is_open()) return cfg;
        cfg.fileFound = true;

        ServerEndpoint legacyServer;
        legacyServer.host = cfg.serverHost;
        legacyServer.port = cfg.serverPort;
        legacyServer.hasHost = true;
        legacyServer.hasPort = true;

        std::unordered_map<std::string, ServerEndpoint> profiles;
        std::string line;
        while (std::getline(f, line)) {
            line = trim(line);
            if (line.empty()) continue;
            if (line[0] == ';' || line[0] == '#' || line[0] == '[') continue;

            auto eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));
            if (key.empty()) continue;

            try {
                if (key == "server_host" && !val.empty()) {
                    legacyServer.host = val;
                    legacyServer.hasHost = true;
                    continue;
                }

                if (key == "server_port" && !val.empty()) {
                    legacyServer.port = std::stoi(val);
                    legacyServer.hasPort = true;
                    continue;
                }

                if (key == "active_server") {
                    cfg.activeServer = val;
                    continue;
                }

                if (assignProfileHost(profiles, key, val)) continue;
                if (assignProfilePort(profiles, key, val)) continue;

                if (key == "api_key") {
                    cfg.apiKey = val;
                    continue;
                }

                if (key == "enabled" && !val.empty()) {
                    cfg.enabled = (val == "1" || val == "true");
                    continue;
                }

                if (key == "debug_log" && !val.empty()) {
                    cfg.debugLog = (val == "1" || val == "true");
                    continue;
                }

                if (key == "smoothing" && !val.empty()) {
                    cfg.smoothAlpha = std::stof(val);
                    continue;
                }

                if (key == "pan_smoothing" && !val.empty()) {
                    cfg.panSmoothAlpha = std::stof(val);
                }
            } catch (...) {
            }
        }

        cfg.applyServerSelection(legacyServer, profiles);
        return cfg;
    }

    static std::string trim(const std::string& s) {
        const char* ws = " \t\r\n";
        size_t a = s.find_first_not_of(ws);
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(ws);
        return s.substr(a, b - a + 1);
    }

private:
    static bool assignProfileHost(std::unordered_map<std::string, ServerEndpoint>& profiles,
                                  const std::string& key,
                                  const std::string& value) {
        std::string profile = parseProfileName(key, "_host");
        if (profile.empty() || value.empty()) return false;
        ServerEndpoint& endpoint = profiles[profile];
        endpoint.host = value;
        endpoint.hasHost = true;
        return true;
    }

    static bool assignProfilePort(std::unordered_map<std::string, ServerEndpoint>& profiles,
                                  const std::string& key,
                                  const std::string& value) {
        std::string profile = parseProfileName(key, "_port");
        if (profile.empty() || value.empty()) return false;
        ServerEndpoint& endpoint = profiles[profile];
        endpoint.port = std::stoi(value);
        endpoint.hasPort = true;
        return true;
    }

    static std::string parseProfileName(const std::string& key, const char* suffix) {
        const std::string suffixText = suffix;
        if (key.size() <= suffixText.size()) return "";
        if (key.rfind("server", 0) != 0) return "";
        if (key.compare(key.size() - suffixText.size(), suffixText.size(), suffixText) != 0) return "";

        const size_t profileLength = key.size() - suffixText.size();
        const std::string profile = key.substr(0, profileLength);
        if (profile.size() <= 6) return "";

        for (size_t i = 6; i < profile.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(profile[i]))) return "";
        }

        return profile;
    }

    void applyServerSelection(const ServerEndpoint& legacyServer,
                              const std::unordered_map<std::string, ServerEndpoint>& profiles) {
        serverHost = legacyServer.host;
        serverPort = legacyServer.port;
        selectedServer = "legacy";

        if (activeServer.empty()) return;

        auto it = profiles.find(activeServer);
        if (it == profiles.end()) return;
        if (!it->second.isValid()) return;

        serverHost = it->second.host;
        serverPort = it->second.port;
        selectedServer = activeServer;
    }
};
