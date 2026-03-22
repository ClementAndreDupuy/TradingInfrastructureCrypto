#pragma once

#include <cstdint>
#include <string>
#ifdef __has_include
#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#else
#include "../../common/json.hpp"
#endif
#else
#include "../../common/json.hpp"
#endif

namespace trading::feed {

struct WsEndpoint {
    std::string host;
    int port;
    std::string path;
};

struct FragmentBuffer {
    char data[131072];
    size_t len{0};
};

auto parse_ws_endpoint(const std::string& ws_url,
                       const std::string& default_host,
                       int default_port,
                       const std::string& default_path) -> WsEndpoint;
auto append_ws_fragment(FragmentBuffer& buffer, const char* fragment, size_t fragment_len,
                        bool is_final) -> std::string;
auto json_uint64(const nlohmann::json& json, const char* key, uint64_t fallback = 0) -> uint64_t;
auto json_int64(const nlohmann::json& json, const char* key, int64_t fallback = 0) -> int64_t;
auto json_string(const nlohmann::json& json, const char* key) -> std::string;
auto parse_rfc3339_timestamp_ns(const std::string& timestamp) -> int64_t;
auto crc32_bytes(const std::string& data) -> uint32_t;

} 
