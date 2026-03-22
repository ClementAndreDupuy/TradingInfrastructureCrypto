#include "coinbase_feed_handler.hpp"
#include <algorithm>
#include <cctype>
#include <exception>

namespace trading {

auto CoinbaseFeedHandler::process_snapshot(const nlohmann::json& json, uint64_t seq) -> Result {
    Snapshot snap;
    snap.symbol = symbol_;
    snap.exchange = Exchange::COINBASE;
    snap.sequence = seq;
    snap.timestamp_local_ns = http::now_ns();
    snap.timestamp_exchange_ns = extract_exchange_timestamp_ns(json);

    auto events_it = json.find("events");
    if (events_it == json.end() || !events_it->is_array()) {
        return Result::ERROR_BOOK_CORRUPTED;
    }

    for (const auto& event : *events_it) {
        auto updates_it = event.find("updates");
        if (updates_it == event.end() || !updates_it->is_array()) {
            continue;
        }

        for (const auto& upd : *updates_it) {
            auto side_it = upd.find("side");
            auto px_it = upd.find("price_level");
            auto qty_it = upd.find("new_quantity");
            if (side_it == upd.end() || px_it == upd.end() || qty_it == upd.end()) {
                continue;
            }

            try {
                PriceLevel level;
                level.price = std::stod(px_it->get<std::string>());
                level.size = std::stod(qty_it->get<std::string>());

                const std::string side = side_it->get<std::string>();
                if (side == "bid") {
                    snap.bids.push_back(level);
                } else if (side == "offer" || side == "ask") {
                    snap.asks.push_back(level);
                }
            } catch (const std::exception& ex) {
                LOG_WARN("[Coinbase] snapshot level parse failed", "symbol", symbol_.c_str(), "error",
                         ex.what());
            }
        }
    }

    if (snap.bids.empty() || snap.asks.empty()) {
        return Result::ERROR_BOOK_CORRUPTED;
    }

    last_sequence_.store(seq, std::memory_order_release);
    last_event_time_exchange_ns_.store(snap.timestamp_exchange_ns, std::memory_order_release);
    if (snapshot_callback_) {
        snapshot_callback_(snap);
    }

    return Result::SUCCESS;
}

auto CoinbaseFeedHandler::process_delta(const nlohmann::json& json, uint64_t seq) -> Result {
    last_sequence_.store(seq, std::memory_order_release);
    const int64_t timestamp_local_ns = http::now_ns();
    const int64_t timestamp_exchange_ns = extract_exchange_timestamp_ns(json);
    last_event_time_exchange_ns_.store(timestamp_exchange_ns, std::memory_order_release);

    auto events_it = json.find("events");
    if (events_it == json.end() || !events_it->is_array()) {
        return Result::SUCCESS;
    }

    for (const auto& event : *events_it) {
        auto updates_it = event.find("updates");
        if (updates_it == event.end() || !updates_it->is_array()) {
            continue;
        }

        for (const auto& upd : *updates_it) {
            auto side_it = upd.find("side");
            auto px_it = upd.find("price_level");
            auto qty_it = upd.find("new_quantity");
            if (side_it == upd.end() || px_it == upd.end() || qty_it == upd.end()) {
                continue;
            }

            try {
                Delta delta;
                delta.side = (side_it->get<std::string>() == "bid") ? Side::BID : Side::ASK;
                delta.price = std::stod(px_it->get<std::string>());
                delta.size = std::stod(qty_it->get<std::string>());
                delta.sequence = seq;
                delta.timestamp_exchange_ns = timestamp_exchange_ns;
                delta.timestamp_local_ns = timestamp_local_ns;
                if (delta_callback_) {
                    delta_callback_(delta);
                }
            } catch (const std::exception& ex) {
                LOG_WARN("[Coinbase] delta level parse failed", "symbol", symbol_.c_str(), "sequence",
                         seq, "error", ex.what());
            }
        }
    }

    return Result::SUCCESS;
}

auto CoinbaseFeedHandler::extract_exchange_timestamp_ns(const nlohmann::json& json) const
    -> int64_t {
    auto ts_it = json.find("timestamp");
    if (ts_it != json.end() && ts_it->is_string()) {
        return parse_rfc3339_ns(ts_it->get<std::string>());
    }

    auto events_it = json.find("events");
    if (events_it != json.end() && events_it->is_array()) {
        for (const auto& event : *events_it) {
            auto event_ts_it = event.find("event_time");
            if (event_ts_it != event.end() && event_ts_it->is_string()) {
                return parse_rfc3339_ns(event_ts_it->get<std::string>());
            }
            auto current_time_it = event.find("current_time");
            if (current_time_it != event.end() && current_time_it->is_string()) {
                return parse_rfc3339_ns(current_time_it->get<std::string>());
            }
        }
    }

    return 0;
}

auto CoinbaseFeedHandler::process_message(const std::string& message) -> Result {
    auto json = nlohmann::json::parse(message, nullptr, false);
    if (json.is_discarded()) {
        return Result::SUCCESS;
    }

    auto type_it = json.find("type");
    if (type_it != json.end() && type_it->is_string() && type_it->get<std::string>() == "error") {
        std::string reason = json.value("message", std::string("unknown error"));
        std::string reason_lower = reason;
        std::transform(reason_lower.begin(), reason_lower.end(), reason_lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (reason_lower.find("auth") != std::string::npos ||
            reason_lower.find("jwt") != std::string::npos ||
            reason_lower.find("signature") != std::string::npos) {
            auth_rejected_.store(true, std::memory_order_release);
            last_start_failure_reason_ = "authentication rejected: " + reason;
        } else {
            subscription_rejected_.store(true, std::memory_order_release);
            last_start_failure_reason_ = "subscription rejected: " + reason;
        }
        running_.store(false, std::memory_order_release);
        ws_cv_.notify_all();
        if (error_callback_) {
            error_callback_(last_start_failure_reason_);
        }
        return Result::ERROR_CONNECTION_LOST;
    }

    auto channel_it = json.find("channel");
    if (channel_it == json.end() || !channel_it->is_string()) {
        return Result::SUCCESS;
    }

    const std::string channel = channel_it->get<std::string>();
    if (channel == "subscriptions") {
        return Result::SUCCESS;
    }
    if (channel == "heartbeats") {
        const int64_t exchange_ts = extract_exchange_timestamp_ns(json);
        last_heartbeat_ns_.store(http::now_ns(), std::memory_order_release);
        if (exchange_ts > 0) {
            last_event_time_exchange_ns_.store(exchange_ts, std::memory_order_release);
        }
        return Result::SUCCESS;
    }
    if (channel != "l2_data") {
        return Result::SUCCESS;
    }

    uint64_t seq = 0;
    auto seq_it = json.find("sequence_num");
    if (seq_it != json.end()) {
        if (seq_it->is_string()) {
            seq = std::stoull(seq_it->get<std::string>());
        } else {
            seq = seq_it->get<uint64_t>();
        }
    }

    bool has_snapshot = false;
    auto events_it = json.find("events");
    if (events_it != json.end() && events_it->is_array()) {
        for (const auto& event : *events_it) {
            auto event_type_it = event.find("type");
            if (event_type_it != event.end() && event_type_it->is_string() &&
                event_type_it->get<std::string>() == "snapshot") {
                has_snapshot = true;
                break;
            }
        }
    }

    if (has_snapshot) {
        Result result = process_snapshot(json, seq);
        if (result != Result::SUCCESS) {
            last_start_failure_reason_ = "invalid snapshot payload";
            trigger_resnapshot("Invalid Coinbase snapshot");
            return result;
        }

        if (apply_buffered_deltas() != Result::SUCCESS) {
            last_start_failure_reason_ = "buffered delta replay failed";
            trigger_resnapshot("Buffered Coinbase deltas invalid after snapshot");
            return Result::ERROR_SEQUENCE_GAP;
        }

        state_.store(State::STREAMING, std::memory_order_release);
        ws_cv_.notify_all();
        return Result::SUCCESS;
    }

    auto cur = state_.load(std::memory_order_acquire);
    if (cur == State::BUFFERING || cur == State::DISCONNECTED) {
        if (delta_buffer_.size() < MAX_BUFFER_SIZE) {
            delta_buffer_.push_back(message);
        } else {
            last_start_failure_reason_ = "buffer overflow before snapshot";
            trigger_resnapshot("Buffer overflow");
        }
        return Result::SUCCESS;
    }

    if (!validate_delta_sequence(seq)) {
        last_start_failure_reason_ = "sequence gap resync";
        trigger_resnapshot("Coinbase sequence gap");
        return Result::ERROR_SEQUENCE_GAP;
    }

    return process_delta(json, seq);
}

auto CoinbaseFeedHandler::apply_buffered_deltas() -> Result {
    size_t applied = 0;
    size_t skipped = 0;

    for (const auto& msg : delta_buffer_) {
        auto json = nlohmann::json::parse(msg, nullptr, false);
        if (json.is_discarded()) {
            continue;
        }

        uint64_t seq = 0;
        auto seq_it = json.find("sequence_num");
        if (seq_it != json.end()) {
            if (seq_it->is_string()) {
                seq = std::stoull(seq_it->get<std::string>());
            } else {
                seq = seq_it->get<uint64_t>();
            }
        }

        uint64_t last = last_sequence_.load(std::memory_order_acquire);
        if (seq <= last) {
            ++skipped;
            continue;
        }

        if (seq != last + 1) {
            delta_buffer_.clear();
            return Result::ERROR_SEQUENCE_GAP;
        }

        if (process_delta(json, seq) == Result::SUCCESS) {
            ++applied;
        }
    }

    delta_buffer_.clear();
    LOG_INFO("[Coinbase] Applied Coinbase buffered deltas", "applied", applied, "skipped", skipped);
    return Result::SUCCESS;
}

auto CoinbaseFeedHandler::validate_delta_sequence(uint64_t seq) const -> bool {
    uint64_t last = last_sequence_.load(std::memory_order_acquire);
    return seq == last + 1;
}

void CoinbaseFeedHandler::trigger_resnapshot(const std::string& reason) {
    LOG_ERROR("[Coinbase] Triggering re-sync", "reason", reason.c_str());
    if (error_callback_) {
        error_callback_("Re-sync: " + reason);
    }
    delta_buffer_.clear();
    state_.store(State::BUFFERING, std::memory_order_release);
    reconnect_requested_.store(true, std::memory_order_release);
    if (auto* ctx = static_cast<struct lws_context*>(lws_ctx_.load(std::memory_order_acquire))) {
        lws_cancel_service(ctx);
    }
}

}
