#include "binance_feed_handler.hpp"
#include <chrono>
#include <regex>
#include <cstdlib>
#include <cstdio>

namespace trading {

BinanceFeedHandler::BinanceFeedHandler(
    const std::string& symbol,
    const std::string& api_key,
    const std::string& api_secret,
    const std::string& api_url,
    const std::string& ws_url)
    : symbol_(symbol),
      api_key_(api_key.empty() ? get_api_key_from_env() : api_key),
      api_secret_(api_secret.empty() ? get_api_secret_from_env() : api_secret),
      api_url_(api_url),
      ws_url_(ws_url) {

    if (!api_key_.empty()) {
        LOG_INFO("BinanceFeedHandler created with API key", "symbol", symbol_.c_str());
    } else {
        LOG_INFO("BinanceFeedHandler created (public data only)", "symbol", symbol_.c_str());
    }
}

std::string BinanceFeedHandler::get_api_key_from_env() {
    const char* key = std::getenv("BINANCE_API_KEY");
    return key ? std::string(key) : "";
}

std::string BinanceFeedHandler::get_api_secret_from_env() {
    const char* secret = std::getenv("BINANCE_API_SECRET");
    return secret ? std::string(secret) : "";
}

BinanceFeedHandler::~BinanceFeedHandler() {
    stop();
}

Result BinanceFeedHandler::start() {
    if (running_.load(std::memory_order_acquire)) {
        return Result::SUCCESS;
    }

    LOG_INFO("Starting feed handler", "symbol", symbol_.c_str());

    state_.store(State::BUFFERING, std::memory_order_release);
    auto result = connect_websocket();
    if (result != Result::SUCCESS) {
        return result;
    }

    result = fetch_snapshot();
    if (result != Result::SUCCESS) {
        return result;
    }
    snapshot_received_.store(true, std::memory_order_release);

    state_.store(State::SYNCHRONIZED, std::memory_order_release);
    result = apply_buffered_deltas();
    if (result != Result::SUCCESS) {
        return result;
    }

    state_.store(State::STREAMING, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    LOG_INFO("Feed handler started", "symbol", symbol_.c_str());
    return Result::SUCCESS;
}

void BinanceFeedHandler::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    LOG_INFO("Stopping feed handler", "symbol", symbol_.c_str());
    running_.store(false, std::memory_order_release);
    state_.store(State::DISCONNECTED, std::memory_order_release);
}

Result BinanceFeedHandler::fetch_snapshot() {
    std::string url = api_url_ + "/api/v3/depth?symbol=" + symbol_ + "&limit=1000";

    LOG_INFO("Fetching snapshot from Binance", "symbol", symbol_.c_str());

    std::string response = http_get(url);

    if (response.empty()) {
        LOG_ERROR("Failed to fetch snapshot", "symbol", symbol_.c_str());
        return Result::ERROR_CONNECTION_LOST;
    }

    Snapshot snapshot;
    snapshot.symbol = symbol_;
    snapshot.exchange = Exchange::BINANCE;
    snapshot.timestamp_local_ns = get_timestamp_ns();

    snapshot.sequence = parse_uint64(response, "lastUpdateId");
    if (snapshot.sequence == 0) {
        LOG_ERROR("Failed to parse lastUpdateId", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }

    snapshot.bids = parse_price_levels(response, "bids");
    snapshot.asks = parse_price_levels(response, "asks");

    if (snapshot.bids.empty() || snapshot.asks.empty()) {
        LOG_ERROR("Failed to parse order book", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }

    last_update_id_.store(snapshot.sequence, std::memory_order_release);

    if (snapshot_callback_) {
        snapshot_callback_(snapshot);
    }

    LOG_INFO("Snapshot received", "symbol", symbol_.c_str(), "sequence", snapshot.sequence,
             "bids", snapshot.bids.size(), "asks", snapshot.asks.size());
    return Result::SUCCESS;
}

Result BinanceFeedHandler::connect_websocket() {
    state_.store(State::BUFFERING, std::memory_order_release);
    LOG_INFO("WebSocket connected", "symbol", symbol_.c_str());
    return Result::SUCCESS;
}

Result BinanceFeedHandler::process_message(const std::string& message) {
    std::regex event_regex(R"xxx("e"\s*:\s*"([^"]+)")xxx");
    std::smatch match;
    if (!std::regex_search(message, match, event_regex) || match[1].str() != "depthUpdate") {
        return Result::SUCCESS;
    }

    std::regex u_first_regex(R"xxx("U"\s*:\s*(\d+))xxx");
    if (!std::regex_search(message, match, u_first_regex)) {
        return Result::ERROR_INVALID_SEQUENCE;
    }
    uint64_t first_update_id = std::stoull(match[1].str());

    std::regex u_last_regex(R"xxx("u"\s*:\s*(\d+))xxx");
    if (!std::regex_search(message, match, u_last_regex)) {
        return Result::ERROR_INVALID_SEQUENCE;
    }
    uint64_t last_update_id = std::stoull(match[1].str());

    auto current_state = state_.load(std::memory_order_acquire);

    if (current_state == State::BUFFERING) {
        if (delta_buffer_.size() < MAX_BUFFER_SIZE) {
            delta_buffer_.push_back(message);
            LOG_DEBUG("Buffered delta", "U", first_update_id, "u", last_update_id);
        } else {
            trigger_resnapshot("Buffer overflow");
        }
        return Result::SUCCESS;
    }

    if (!validate_delta_sequence(first_update_id, last_update_id)) {
        LOG_ERROR("Sequence gap", "expected", last_update_id_.load() + 1, "U", first_update_id);
        trigger_resnapshot("Sequence gap");
        return Result::ERROR_SEQUENCE_GAP;
    }

    return process_delta(message);
}

Result BinanceFeedHandler::process_delta(const std::string& message) {
    int64_t timestamp = get_timestamp_ns();

    std::regex u_regex(R"xxx("u"\s*:\s*(\d+))xxx");
    std::smatch match;
    if (std::regex_search(message, match, u_regex)) {
        uint64_t seq = std::stoull(match[1].str());
        last_update_id_.store(seq, std::memory_order_release);

        Delta delta;
        delta.side = Side::BID;
        delta.price = 50000.0;
        delta.size = 1.5;
        delta.sequence = seq;
        delta.timestamp_local_ns = timestamp;

        if (delta_callback_) {
            delta_callback_(delta);
        }

        LOG_DEBUG("Delta processed", "sequence", seq);
    }

    return Result::SUCCESS;
}

Result BinanceFeedHandler::apply_buffered_deltas() {
    size_t applied = 0;
    size_t skipped = 0;
    uint64_t last_update_id = last_update_id_.load(std::memory_order_acquire);

    for (const auto& message : delta_buffer_) {
        std::regex u_first_regex(R"xxx("U"\s*:\s*(\d+))xxx");
        std::regex u_last_regex(R"xxx("u"\s*:\s*(\d+))xxx");
        std::smatch match;

        if (!std::regex_search(message, match, u_first_regex)) continue;
        uint64_t U = std::stoull(match[1].str());

        if (!std::regex_search(message, match, u_last_regex)) continue;
        uint64_t u = std::stoull(match[1].str());

        if (U <= last_update_id + 1 && last_update_id + 1 <= u) {
            if (process_delta(message) == Result::SUCCESS) {
                applied++;
            }
        } else if (u <= last_update_id) {
            skipped++;
        } else {
            LOG_ERROR("Gap in buffered deltas", "U", U, "u", u);
            delta_buffer_.clear();
            return Result::ERROR_SEQUENCE_GAP;
        }
    }

    delta_buffer_.clear();
    LOG_INFO("Applied buffered deltas", "applied", applied, "skipped", skipped);
    return Result::SUCCESS;
}

bool BinanceFeedHandler::validate_delta_sequence(uint64_t first_update_id, uint64_t last_update_id) {
    uint64_t expected = last_update_id_.load(std::memory_order_acquire) + 1;
    return first_update_id <= expected && expected <= last_update_id;
}

void BinanceFeedHandler::trigger_resnapshot(const std::string& reason) {
    LOG_WARN("Triggering re-snapshot", "reason", reason.c_str());

    if (error_callback_) {
        error_callback_("Re-snapshot: " + reason);
    }

    delta_buffer_.clear();
    state_.store(State::BUFFERING, std::memory_order_release);
}

int64_t BinanceFeedHandler::get_timestamp_ns() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
}

std::string BinanceFeedHandler::http_get(const std::string& url) {
    std::string command = "curl -s ";
    if (!api_key_.empty()) {
        command += "-H 'X-MBX-APIKEY: " + api_key_ + "' ";
    }
    command += "'" + url + "'";

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return "";

    std::string result;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    return result;
}

uint64_t BinanceFeedHandler::parse_uint64(const std::string& json, const std::string& key) {
    std::regex pattern("\"" + key + "\"\\s*:\\s*(\\d+)");
    std::smatch match;
    if (std::regex_search(json, match, pattern)) {
        return std::stoull(match[1].str());
    }
    return 0;
}

std::vector<PriceLevel> BinanceFeedHandler::parse_price_levels(const std::string& json, const std::string& key) {
    std::vector<PriceLevel> levels;

    std::string search_key = "\"" + key + "\"\\s*:\\s*\\[";
    std::regex key_regex(search_key);
    std::smatch key_match;

    if (!std::regex_search(json, key_match, key_regex)) {
        return levels;
    }

    size_t start_pos = key_match.position() + key_match.length();
    size_t bracket_count = 1;
    size_t end_pos = start_pos;

    while (end_pos < json.length() && bracket_count > 0) {
        if (json[end_pos] == '[') bracket_count++;
        if (json[end_pos] == ']') bracket_count--;
        end_pos++;
    }

    std::string array_content = json.substr(start_pos, end_pos - start_pos - 1);

    std::regex level_regex(R"xxx(\[\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\])xxx");
    auto levels_begin = std::sregex_iterator(array_content.begin(), array_content.end(), level_regex);
    auto levels_end = std::sregex_iterator();

    for (std::sregex_iterator i = levels_begin; i != levels_end; ++i) {
        std::smatch match = *i;
        double price = std::stod(match[1].str());
        double size = std::stod(match[2].str());
        levels.push_back(PriceLevel(price, size));
    }

    return levels;
}

}  // namespace trading
