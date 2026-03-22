#include "feed_handler_utils.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace trading::feed {

auto parse_ws_endpoint(const std::string& ws_url, const std::string& default_host,
                       int default_port, const std::string& default_path) -> WsEndpoint {
    WsEndpoint endpoint{default_host, default_port, default_path};
    size_t pos = ws_url.find("://");
    pos = (pos == std::string::npos) ? 0 : pos + 3;
    const size_t slash = ws_url.find('/', pos);
    const std::string authority =
        ws_url.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
    if (slash != std::string::npos) {
        endpoint.path = ws_url.substr(slash);
    }
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        endpoint.host = authority.substr(0, colon);
        endpoint.port = std::stoi(authority.substr(colon + 1));
    } else if (!authority.empty()) {
        endpoint.host = authority;
    }
    return endpoint;
}

auto append_ws_fragment(FragmentBuffer& buffer, const char* fragment, size_t fragment_len,
                        bool is_final) -> std::string {
    if (buffer.len + fragment_len > sizeof(buffer.data)) {
        buffer.len = 0;
        return {};
    }
    std::memcpy(buffer.data + buffer.len, fragment, fragment_len);
    buffer.len += fragment_len;
    if (!is_final || buffer.len == 0) {
        return {};
    }
    std::string message(buffer.data, buffer.len);
    buffer.len = 0;
    return message;
}

auto json_uint64(const nlohmann::json& json, const char* key, uint64_t fallback) -> uint64_t {
    auto it = json.find(key);
    if (it == json.end()) {
        return fallback;
    }
    if (it->is_string()) {
        return static_cast<uint64_t>(std::stoull(it->get<std::string>()));
    }
    if (it->is_number_unsigned() || it->is_number_integer()) {
        return it->get<uint64_t>();
    }
    return fallback;
}

auto json_int64(const nlohmann::json& json, const char* key, int64_t fallback) -> int64_t {
    auto it = json.find(key);
    if (it == json.end()) {
        return fallback;
    }
    if (it->is_string()) {
        return static_cast<int64_t>(std::stoll(it->get<std::string>()));
    }
    if (it->is_number_integer() || it->is_number_unsigned()) {
        return it->get<int64_t>();
    }
    return fallback;
}

auto json_string(const nlohmann::json& json, const char* key) -> std::string {
    auto it = json.find(key);
    if (it == json.end()) {
        return {};
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    return it->dump();
}

auto parse_rfc3339_timestamp_ns(const std::string& timestamp) -> int64_t {
    if (timestamp.size() < 19U) {
        return 0;
    }
    std::tm tm = {};
    std::istringstream stream(timestamp.substr(0, 19));
    stream >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (stream.fail()) {
        return 0;
    }
#if defined(_WIN32)
    const std::time_t seconds = _mkgmtime(&tm);
#else
    const std::time_t seconds = timegm(&tm);
#endif
    if (seconds < 0) {
        return 0;
    }
    int64_t nanos = 0;
    size_t pos = 19U;
    if (pos < timestamp.size() && timestamp[pos] == '.') {
        ++pos;
        size_t digits = 0;
        while (pos < timestamp.size() && std::isdigit(static_cast<unsigned char>(timestamp[pos])) &&
               digits < 9U) {
            nanos = nanos * 10 + static_cast<int64_t>(timestamp[pos] - '0');
            ++pos;
            ++digits;
        }
        while (digits < 9U) {
            nanos *= 10;
            ++digits;
        }
    }
    return static_cast<int64_t>(seconds) * 1000000000LL + nanos;
}

auto crc32_bytes(const std::string& data) -> uint32_t {
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char byte : data) {
        crc ^= static_cast<uint32_t>(byte);
        for (int i = 0; i < 8; ++i) {
            const uint32_t mask = static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1u)));
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

}
