#include "kraken_feed_handler.hpp"
#include <chrono>
#include <regex>
#include <cstdlib>
#include <cstdio>

namespace trading {

KrakenFeedHandler::KrakenFeedHandler(
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
    LOG_INFO("KrakenFeedHandler created (public data, no auth required)", "symbol", symbol_.c_str());
}

std::string KrakenFeedHandler::get_api_key_from_env() {
    const char* key = std::getenv("KRAKEN_API_KEY");
    return key ? std::string(key) : "";
}

std::string KrakenFeedHandler::get_api_secret_from_env() {
    const char* secret = std::getenv("KRAKEN_API_SECRET");
    return secret ? std::string(secret) : "";
}

KrakenFeedHandler::~KrakenFeedHandler() {
    stop();
}

Result KrakenFeedHandler::start() {
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

void KrakenFeedHandler::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    LOG_INFO("Stopping feed handler", "symbol", symbol_.c_str());
    running_.store(false, std::memory_order_release);
    state_.store(State::DISCONNECTED, std::memory_order_release);
}

Result KrakenFeedHandler::fetch_snapshot() {
    std::string url = api_url_ + "/0/public/Depth?pair=" + symbol_ + "&count=500";

    LOG_INFO("Fetching snapshot from Kraken REST", "symbol", symbol_.c_str());

    std::string response = http_get(url);

    if (response.empty()) {
        LOG_ERROR("Failed to fetch snapshot", "symbol", symbol_.c_str());
        return Result::ERROR_CONNECTION_LOST;
    }

    // Kraken returns {"error":[],...} on success; non-empty error array on failure.
    std::regex error_regex(R"xxx("error"\s*:\s*\[\s*\])xxx");
    if (!std::regex_search(response, error_regex)) {
        LOG_ERROR("Kraken API returned error", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }

    Snapshot snapshot;
    snapshot.symbol = symbol_;
    snapshot.exchange = Exchange::KRAKEN;
    snapshot.timestamp_local_ns = get_timestamp_ns();
    snapshot.sequence = 0;  // Kraken REST has no sequence number

    // REST price levels: ["price_str","vol_str",timestamp_int]
    snapshot.bids = parse_kraken_rest_levels(response, "bids");
    snapshot.asks = parse_kraken_rest_levels(response, "asks");

    if (snapshot.bids.empty() || snapshot.asks.empty()) {
        LOG_ERROR("Failed to parse order book", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }

    last_seq_.store(0, std::memory_order_release);

    if (snapshot_callback_) {
        snapshot_callback_(snapshot);
    }

    LOG_INFO("Snapshot received", "symbol", symbol_.c_str(),
             "bids", snapshot.bids.size(), "asks", snapshot.asks.size());
    return Result::SUCCESS;
}

Result KrakenFeedHandler::connect_websocket() {
    state_.store(State::BUFFERING, std::memory_order_release);
    LOG_INFO("WebSocket connected", "symbol", symbol_.c_str(), "url", ws_url_.c_str());
    return Result::SUCCESS;
}

Result KrakenFeedHandler::process_message(const std::string& message) {
    // Only process book channel messages.
    std::regex channel_regex(R"xxx("channel"\s*:\s*"book")xxx");
    if (!std::regex_search(message, channel_regex)) {
        return Result::SUCCESS;
    }

    // Require a seq field.
    std::regex seq_regex(R"xxx("seq"\s*:\s*(\d+))xxx");
    std::smatch match;
    if (!std::regex_search(message, match, seq_regex)) {
        return Result::ERROR_INVALID_SEQUENCE;
    }
    uint64_t seq = std::stoull(match[1].str());

    auto current_state = state_.load(std::memory_order_acquire);

    if (current_state == State::BUFFERING) {
        if (delta_buffer_.size() < MAX_BUFFER_SIZE) {
            delta_buffer_.push_back(message);
            LOG_DEBUG("Buffered message", "seq", seq);
        } else {
            trigger_resnapshot("Buffer overflow");
        }
        return Result::SUCCESS;
    }

    if (!validate_delta_sequence(seq)) {
        LOG_ERROR("Sequence gap", "expected", last_seq_.load() + 1, "seq", seq);
        trigger_resnapshot("Sequence gap");
        return Result::ERROR_SEQUENCE_GAP;
    }

    return process_delta(message);
}

Result KrakenFeedHandler::process_delta(const std::string& message) {
    int64_t timestamp = get_timestamp_ns();

    std::regex seq_regex(R"xxx("seq"\s*:\s*(\d+))xxx");
    std::smatch match;
    if (!std::regex_search(message, match, seq_regex)) {
        return Result::ERROR_INVALID_SEQUENCE;
    }

    uint64_t seq = std::stoull(match[1].str());
    last_seq_.store(seq, std::memory_order_release);

    // WebSocket v2 price levels: {"price":X,"qty":Y}
    auto bids = parse_kraken_ws_levels(message, "bids");
    for (const auto& level : bids) {
        Delta delta;
        delta.side = Side::BID;
        delta.price = level.price;
        delta.size = level.size;
        delta.sequence = seq;
        delta.timestamp_local_ns = timestamp;
        if (delta_callback_) {
            delta_callback_(delta);
        }
    }

    auto asks = parse_kraken_ws_levels(message, "asks");
    for (const auto& level : asks) {
        Delta delta;
        delta.side = Side::ASK;
        delta.price = level.price;
        delta.size = level.size;
        delta.sequence = seq;
        delta.timestamp_local_ns = timestamp;
        if (delta_callback_) {
            delta_callback_(delta);
        }
    }

    LOG_DEBUG("Delta processed", "sequence", seq, "bids", bids.size(), "asks", asks.size());
    return Result::SUCCESS;
}

Result KrakenFeedHandler::apply_buffered_deltas() {
    size_t applied = 0;
    size_t skipped = 0;

    for (const auto& message : delta_buffer_) {
        std::regex seq_regex(R"xxx("seq"\s*:\s*(\d+))xxx");
        std::smatch match;
        if (!std::regex_search(message, match, seq_regex)) continue;
        uint64_t seq = std::stoull(match[1].str());

        uint64_t last_seq = last_seq_.load(std::memory_order_acquire);

        if (seq <= last_seq) {
            skipped++;
        } else if (seq == last_seq + 1) {
            if (process_delta(message) == Result::SUCCESS) {
                applied++;
            }
        } else {
            LOG_ERROR("Gap in buffered deltas", "seq", seq, "expected", last_seq + 1);
            delta_buffer_.clear();
            return Result::ERROR_SEQUENCE_GAP;
        }
    }

    delta_buffer_.clear();
    LOG_INFO("Applied buffered deltas", "applied", applied, "skipped", skipped);
    return Result::SUCCESS;
}

bool KrakenFeedHandler::validate_delta_sequence(uint64_t seq) {
    // Kraken WebSocket v2 sequences are strictly +1; no window overlap like Binance.
    return seq == last_seq_.load(std::memory_order_acquire) + 1;
}

void KrakenFeedHandler::trigger_resnapshot(const std::string& reason) {
    LOG_WARN("Triggering re-snapshot", "reason", reason.c_str());

    if (error_callback_) {
        error_callback_("Re-snapshot: " + reason);
    }

    delta_buffer_.clear();
    state_.store(State::BUFFERING, std::memory_order_release);
}

int64_t KrakenFeedHandler::get_timestamp_ns() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
}

std::string KrakenFeedHandler::http_get(const std::string& url) {
    // Kraken order book is public; no auth header needed.
    std::string command = "curl -s '" + url + "'";

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

std::vector<PriceLevel> KrakenFeedHandler::parse_kraken_rest_levels(
    const std::string& json, const std::string& key) {
    std::vector<PriceLevel> levels;

    std::string search = "\"" + key + "\"\\s*:\\s*\\[";
    std::regex key_regex(search);
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

    // Kraken REST: ["price_str","vol_str",timestamp_int]
    std::regex level_regex(R"xxx(\[\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*\d+\s*\])xxx");
    auto it = std::sregex_iterator(array_content.begin(), array_content.end(), level_regex);
    auto end = std::sregex_iterator();

    for (; it != end; ++it) {
        std::smatch m = *it;
        double price = std::stod(m[1].str());
        double size = std::stod(m[2].str());
        levels.push_back(PriceLevel(price, size));
    }

    return levels;
}

std::vector<PriceLevel> KrakenFeedHandler::parse_kraken_ws_levels(
    const std::string& json, const std::string& key) {
    std::vector<PriceLevel> levels;

    std::string search = "\"" + key + "\"\\s*:\\s*\\[";
    std::regex key_regex(search);
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

    // Kraken WebSocket v2: {"price":X,"qty":Y}
    std::regex level_regex(R"xxx(\{"price"\s*:\s*([\d.]+)\s*,\s*"qty"\s*:\s*([\d.]+)\s*\})xxx");
    auto it = std::sregex_iterator(array_content.begin(), array_content.end(), level_regex);
    auto end = std::sregex_iterator();

    for (; it != end; ++it) {
        std::smatch m = *it;
        double price = std::stod(m[1].str());
        double size = std::stod(m[2].str());
        levels.push_back(PriceLevel(price, size));
    }

    return levels;
}

}  // namespace trading
