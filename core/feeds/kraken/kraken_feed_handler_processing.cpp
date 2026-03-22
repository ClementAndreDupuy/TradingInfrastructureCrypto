#include "kraken_feed_handler.hpp"
#include "../common/feed_handler_utils.hpp"
#include <ctime>
#include <exception>
#include <iomanip>
#include <regex>
#include <sstream>

namespace trading {

uint32_t KrakenFeedHandler::crc32_for_test(const std::string& data) { return feed::crc32_bytes(data); }

auto KrakenFeedHandler::process_message(const std::string& message) -> Result {
    auto json = nlohmann::json::parse(message, nullptr, false);
    if (json.is_discarded()) {
        return Result::SUCCESS;
    }

    auto method_it = json.find("method");
    if (method_it != json.end() && method_it->is_string()) {
        const std::string method = method_it->get<std::string>();
        if (method == "subscribe") {
            auto success_it = json.find("success");
            if (success_it != json.end() && success_it->is_boolean() && !success_it->get<bool>()) {
                trigger_resnapshot(json.value("error", std::string("Kraken subscribe rejected")));
                return Result::ERROR_CONNECTION_LOST;
            }
            return Result::SUCCESS;
        }
        return Result::SUCCESS;
    }

    auto ch_it = json.find("channel");
    if (ch_it == json.end() || !ch_it->is_string() || ch_it->get<std::string>() != "book") {
        return Result::SUCCESS;
    }

    const std::string type = json.value("type", std::string());
    if (type == "snapshot") {
        return process_snapshot(message, json);
    }
    if (type != "update") {
        return Result::SUCCESS;
    }

    const uint64_t seq = last_sequence_.load(std::memory_order_acquire) + 1;
    auto cur = state_.load(std::memory_order_acquire);
    if (cur == State::BUFFERING) {
        if (delta_buffer_.size() < MAX_BUFFER_SIZE) {
            delta_buffer_.push_back(message);
        } else {
            trigger_resnapshot("Buffer overflow");
        }
        return Result::SUCCESS;
    }

    return process_delta(message, json, seq);
}

auto KrakenFeedHandler::process_snapshot(const std::string& message,
                                         const nlohmann::json& json) -> Result {
    auto data_it = json.find("data");
    if (data_it == json.end() || !data_it->is_array() || data_it->empty()) {
        return Result::ERROR_BOOK_CORRUPTED;
    }

    const auto& book = (*data_it)[0];
    bids_.clear();
    asks_.clear();
    apply_local_book_levels(book, message);
    truncate_book();

    if (!validate_checksum(book)) {
        trigger_resnapshot("Checksum mismatch");
        return Result::ERROR_BOOK_CORRUPTED;
    }

    Snapshot snap;
    snap.symbol = symbol_;
    snap.exchange = Exchange::KRAKEN;
    snap.sequence = 0;
    snap.timestamp_exchange_ns = parse_rfc3339_timestamp_ns(book.value("timestamp", std::string()));
    snap.timestamp_local_ns = http::now_ns();
    snap.bids.reserve(bids_.size());
    for (const auto& [price, level] : bids_) {
        snap.bids.emplace_back(price, std::stod(level.second));
    }
    snap.asks.reserve(asks_.size());
    for (const auto& [price, level] : asks_) {
        snap.asks.emplace_back(price, std::stod(level.second));
    }

    if (snap.bids.empty() || snap.asks.empty()) {
        return Result::ERROR_BOOK_CORRUPTED;
    }

    last_sequence_.store(0, std::memory_order_release);
    if (snapshot_callback_) {
        snapshot_callback_(snap);
    }

    if (apply_buffered_deltas() != Result::SUCCESS) {
        trigger_resnapshot("Buffered delta replay failed");
        return Result::ERROR_SEQUENCE_GAP;
    }

    state_.store(State::STREAMING, std::memory_order_release);
    ws_cv_.notify_all();
    return Result::SUCCESS;
}

auto KrakenFeedHandler::process_delta(const std::string& message, const nlohmann::json& json,
                                      uint64_t seq) -> Result {
    auto data_it = json.find("data");
    if (data_it == json.end() || !data_it->is_array() || data_it->empty()) {
        return Result::ERROR_BOOK_CORRUPTED;
    }

    const auto& book = (*data_it)[0];
    apply_local_book_levels(book, message);
    truncate_book();

    if (!validate_checksum(book)) {
        LOG_ERROR("[Kraken] checksum mismatch", "symbol", symbol_.c_str());
        trigger_resnapshot("Checksum mismatch");
        return Result::ERROR_BOOK_CORRUPTED;
    }

    const int64_t timestamp_local_ns = http::now_ns();
    const int64_t timestamp_exchange_ns =
        parse_rfc3339_timestamp_ns(book.value("timestamp", std::string()));

    auto emit_levels = [&](const nlohmann::json& arr, Side side) -> Result {
        if (!arr.is_array()) {
            return Result::SUCCESS;
        }
        for (const auto& lvl : arr) {
            auto p_it = lvl.find("price");
            auto q_it = lvl.find("qty");
            if (p_it == lvl.end() || q_it == lvl.end()) {
                continue;
            }
            Delta delta;
            delta.side = side;
            delta.price = std::stod(json_number_to_wire_string(*p_it));
            delta.size = std::stod(json_number_to_wire_string(*q_it));
            delta.sequence = seq;
            delta.timestamp_exchange_ns = timestamp_exchange_ns;
            delta.timestamp_local_ns = timestamp_local_ns;
            if (delta_callback_) {
                delta_callback_(delta);
            }
        }
        return Result::SUCCESS;
    };

    if (emit_levels(book.value("bids", nlohmann::json::array()), Side::BID) != Result::SUCCESS ||
        emit_levels(book.value("asks", nlohmann::json::array()), Side::ASK) != Result::SUCCESS) {
        return Result::ERROR_BOOK_CORRUPTED;
    }

    last_sequence_.store(seq, std::memory_order_release);
    return Result::SUCCESS;
}

auto KrakenFeedHandler::apply_buffered_deltas() -> Result {
    uint64_t next_seq = 1;
    for (const auto& msg : delta_buffer_) {
        auto json = nlohmann::json::parse(msg, nullptr, false);
        if (json.is_discarded()) {
            continue;
        }
        if (process_delta(msg, json, next_seq) != Result::SUCCESS) {
            delta_buffer_.clear();
            return Result::ERROR_BOOK_CORRUPTED;
        }
        ++next_seq;
    }

    delta_buffer_.clear();
    return Result::SUCCESS;
}

bool KrakenFeedHandler::validate_checksum(const nlohmann::json& data) const {
    auto checksum_it = data.find("checksum");
    if (checksum_it == data.end()) {
        return true;
    }

    std::string payload;
    payload.reserve(512);

    size_t level_count = 0;
    for (auto it = asks_.begin(); it != asks_.end() && level_count < 10; ++it, ++level_count) {
        payload += normalize_checksum_field(it->second.first);
        payload += normalize_checksum_field(it->second.second);
    }

    level_count = 0;
    for (auto it = bids_.begin(); it != bids_.end() && level_count < 10; ++it, ++level_count) {
        payload += normalize_checksum_field(it->second.first);
        payload += normalize_checksum_field(it->second.second);
    }

    return feed::crc32_bytes(payload) == checksum_it->get<uint32_t>();
}

void KrakenFeedHandler::apply_local_book_levels(const nlohmann::json& data,
                                                const std::string& message) {
    const auto raw_bids = extract_levels_from_message(message, "bids");
    const auto raw_asks = extract_levels_from_message(message, "asks");

    auto apply_side = [](const nlohmann::json& arr,
                         const std::vector<std::pair<std::string, std::string>>& raw_levels,
                         auto& book_side) {
        if (!arr.is_array()) {
            return;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            const auto& lvl = arr[i];
            auto price_it = lvl.find("price");
            auto qty_it = lvl.find("qty");
            if (price_it == lvl.end() || qty_it == lvl.end()) {
                continue;
            }
            const std::string price_str = i < raw_levels.size()
                                              ? raw_levels[i].first
                                              : KrakenFeedHandler::json_number_to_wire_string(
                                                    *price_it);
            const std::string qty_str = i < raw_levels.size()
                                            ? raw_levels[i].second
                                            : KrakenFeedHandler::json_number_to_wire_string(*qty_it);
            const double price = std::stod(price_str);
            const double qty = std::stod(qty_str);

            if (qty == 0.0) {
                book_side.erase(price);
            } else {
                book_side[price] = {price_str, qty_str};
            }
        }
    };

    apply_side(data.value("bids", nlohmann::json::array()), raw_bids, bids_);
    apply_side(data.value("asks", nlohmann::json::array()), raw_asks, asks_);
}

void KrakenFeedHandler::truncate_book() {
    while (bids_.size() > subscribed_depth_) {
        auto it = bids_.end();
        --it;
        bids_.erase(it);
    }
    while (asks_.size() > subscribed_depth_) {
        auto it = asks_.end();
        --it;
        asks_.erase(it);
    }
}

int64_t KrakenFeedHandler::parse_rfc3339_timestamp_ns(const std::string& timestamp) {
    return feed::parse_rfc3339_timestamp_ns(timestamp);
}

std::string KrakenFeedHandler::json_number_to_wire_string(const nlohmann::json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return value.dump();
}

std::string KrakenFeedHandler::normalize_checksum_field(const nlohmann::json& value) {
    return normalize_checksum_field(json_number_to_wire_string(value));
}

std::string KrakenFeedHandler::normalize_checksum_field(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        if (ch != '.') {
            out.push_back(ch);
        }
    }
    const size_t first_non_zero = out.find_first_not_of('0');
    if (first_non_zero == std::string::npos) {
        return "0";
    }
    return out.substr(first_non_zero);
}

std::vector<std::pair<std::string, std::string>>
KrakenFeedHandler::extract_levels_from_message(const std::string& message, const std::string& side) {
    const std::string key = "\"" + side + "\"";
    const size_t key_pos = message.find(key);
    if (key_pos == std::string::npos) {
        return {};
    }

    const size_t array_start = message.find('[', key_pos);
    if (array_start == std::string::npos) {
        return {};
    }

    size_t depth = 0;
    size_t array_end = std::string::npos;
    for (size_t i = array_start; i < message.size(); ++i) {
        if (message[i] == '[') {
            ++depth;
        } else if (message[i] == ']') {
            --depth;
            if (depth == 0) {
                array_end = i;
                break;
            }
        }
    }
    if (array_end == std::string::npos) {
        return {};
    }

    const std::string body = message.substr(array_start, array_end - array_start + 1);
    static const std::regex level_pattern(
        R"regex("price"\s*:\s*("?[-+0-9.eE]+"?)\s*,\s*"qty"\s*:\s*("?[-+0-9.eE]+"?))regex");

    std::vector<std::pair<std::string, std::string>> levels;
    for (std::sregex_iterator it(body.begin(), body.end(), level_pattern), end; it != end; ++it) {
        auto unquote = [](std::string value) {
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                return value.substr(1, value.size() - 2);
            }
            return value;
        };
        levels.emplace_back(unquote((*it)[1].str()), unquote((*it)[2].str()));
    }
    return levels;
}

void KrakenFeedHandler::trigger_resnapshot(const std::string& reason) {
    LOG_WARN("[Kraken] Triggering re-snapshot", "reason", reason.c_str());
    if (error_callback_) {
        error_callback_("Re-snapshot: " + reason);
    }
    delta_buffer_.clear();
    bids_.clear();
    asks_.clear();
    last_sequence_.store(0, std::memory_order_release);
    state_.store(State::BUFFERING, std::memory_order_release);
    force_reconnect_.store(true, std::memory_order_release);
    if (auto* ctx = static_cast<struct lws_context*>(lws_ctx_.load(std::memory_order_acquire))) {
        lws_cancel_service(ctx);
    }
}

}
