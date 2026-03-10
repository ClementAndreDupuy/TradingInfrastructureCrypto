#pragma once

// Shared HTTP + JSON parsing utilities for all feed handlers and connectors.
// HTTP is delegated to curl via popen() — consistent with the codebase.
// All are free functions in trading::http (header-only).

#include <string>
#include <vector>
#include <chrono>
#include <regex>
#include <cstdio>
#include <cstdlib>

namespace trading {
namespace http {

// ── HTTP helpers ──────────────────────────────────────────────────────────────

inline std::string get(const std::string& url,
                       const std::vector<std::string>& headers = {}) {
    std::string cmd = "curl -s";
    for (const auto& h : headers) cmd += " -H '" + h + "'";
    cmd += " '" + url + "'";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string result; char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe); return result;
}

inline std::string post(const std::string& url,
                        const std::string& body = "",
                        const std::vector<std::string>& headers = {}) {
    std::string cmd = "curl -s -X POST";
    for (const auto& h : headers) cmd += " -H '" + h + "'";
    if (!body.empty()) cmd += " -d '" + body + "'";
    cmd += " '" + url + "'";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string result; char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe); return result;
}

inline std::string put(const std::string& url,
                       const std::string& body = "",
                       const std::vector<std::string>& headers = {}) {
    std::string cmd = "curl -s -X PUT";
    for (const auto& h : headers) cmd += " -H '" + h + "'";
    if (!body.empty()) cmd += " -d '" + body + "'";
    cmd += " '" + url + "'";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string result; char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe); return result;
}

inline std::string del(const std::string& url,
                       const std::vector<std::string>& headers = {}) {
    std::string cmd = "curl -s -X DELETE";
    for (const auto& h : headers) cmd += " -H '" + h + "'";
    cmd += " '" + url + "'";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string result; char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe); return result;
}

// ── JSON parsing ──────────────────────────────────────────────────────────────

inline std::string parse_string(const std::string& json, const std::string& key) {
    std::regex re("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch m;
    return std::regex_search(json, m, re) ? m[1].str() : "";
}

inline double parse_double(const std::string& json, const std::string& key) {
    std::regex re("\"" + key + "\"\\s*:\\s*\"?([0-9.eE+\\-]+)\"?");
    std::smatch m;
    if (!std::regex_search(json, m, re)) return 0.0;
    try { return std::stod(m[1].str()); } catch (...) { return 0.0; }
}

inline uint64_t parse_uint64(const std::string& json, const std::string& key) {
    std::regex re("\"" + key + "\"\\s*:\\s*(\\d+)");
    std::smatch m;
    if (!std::regex_search(json, m, re)) return 0ULL;
    try { return std::stoull(m[1].str()); } catch (...) { return 0ULL; }
}

inline int64_t parse_int64(const std::string& json, const std::string& key) {
    std::regex re("\"" + key + "\"\\s*:\\s*([0-9]+)");
    std::smatch m;
    if (!std::regex_search(json, m, re)) return 0LL;
    try { return std::stoll(m[1].str()); } catch (...) { return 0LL; }
}

// ── Timestamps ────────────────────────────────────────────────────────────────

inline int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ── Credentials ───────────────────────────────────────────────────────────────

inline std::string env_var(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

}  // namespace http
}  // namespace trading
