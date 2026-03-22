#include "okx_feed_handler.hpp"
#include "../common/feed_handler_utils.hpp"
#include <exception>
#include <iomanip>
#include <sstream>

namespace trading {

uint32_t OkxFeedHandler::crc32_for_test(const std::string& data) { return feed::crc32_bytes(data); }

auto OkxFeedHandler::process_snapshot(const nlohmann::json& json) -> Result {
    auto data_it = json.find("data");
    if (data_it == json.end() || !data_it->is_array() || data_it->empty()) {
        return Result::SUCCESS;
    }

    const auto& book = (*data_it)[0];

    uint64_t seq = 0;
    auto seq_it = book.find("seqId");
    if (seq_it != book.end()) {
        if (seq_it->is_string()) {
            seq = std::stoull(seq_it->get<std::string>());
        } else {
            seq = seq_it->get<uint64_t>();
        }
    }

    const char* sym = symbol_.c_str();
    auto parse_levels = [sym](const nlohmann::json& arr) -> std::vector<PriceLevel> {
        std::vector<PriceLevel> out;
        if (!arr.is_array()) {
            return out;
        }
        out.reserve(arr.size());
        for (const auto& lvl : arr) {
            if (!lvl.is_array() || lvl.size() < 2) {
                continue;
            }
            try {
                out.push_back(
                    {std::stod(lvl[0].get<std::string>()), std::stod(lvl[1].get<std::string>())});
            } catch (const std::exception& ex) {
                LOG_WARN("[OKX] WS snapshot level parse failed", "symbol", sym, "error", ex.what());
            }
        }
        return out;
    };

    Snapshot snap;
    snap.symbol = symbol_;
    snap.exchange = Exchange::OKX;
    snap.timestamp_local_ns = http::now_ns();
    snap.sequence = seq;
    snap.bids = parse_levels(book.value("bids", nlohmann::json::array()));
    snap.asks = parse_levels(book.value("asks", nlohmann::json::array()));

    if (snap.bids.empty() || snap.asks.empty()) {
        LOG_ERROR("[OKX] OKX WS snapshot has empty book", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }

    bids_.clear();
    asks_.clear();
    apply_local_book_levels(book);
    last_sequence_.store(seq, std::memory_order_release);

    if (snapshot_callback_) {
        snapshot_callback_(snap);
    }

    if (apply_buffered_deltas() != Result::SUCCESS) {
        trigger_resnapshot("Failed to apply buffered deltas after WS snapshot");
        return Result::ERROR_BOOK_CORRUPTED;
    }

    state_.store(State::STREAMING, std::memory_order_release);
    ws_cv_.notify_all();

    LOG_INFO("[OKX] WS snapshot applied", "symbol", symbol_.c_str(), "seq", seq);
    return Result::SUCCESS;
}

auto OkxFeedHandler::process_delta(const nlohmann::json& json, uint64_t seq) -> Result {
    last_sequence_.store(seq, std::memory_order_release);
    int64_t timestamp_ns = http::now_ns();

    const nlohmann::json* src = &json;
    auto data_it = json.find("data");
    if (data_it != json.end() && data_it->is_array() && !data_it->empty()) {
        src = &(*data_it)[0];
    }

    apply_local_book_levels(*src);

    auto emit_levels = [&](const nlohmann::json& arr, Side side) -> void {
        if (!arr.is_array()) {
            return;
        }
        for (const auto& lvl : arr) {
            if (!lvl.is_array() || lvl.size() < 2) {
                continue;
            }
            Delta delta;
            delta.side = side;
            delta.price = std::stod(lvl[0].get<std::string>());
            delta.size = std::stod(lvl[1].get<std::string>());
            delta.sequence = seq;
            delta.timestamp_local_ns = timestamp_ns;
            if (delta_callback_) {
                delta_callback_(delta);
            }
        }
    };

    emit_levels(src->value("bids", nlohmann::json::array()), Side::BID);
    emit_levels(src->value("asks", nlohmann::json::array()), Side::ASK);

    return Result::SUCCESS;
}

auto OkxFeedHandler::process_message(const std::string& message) -> Result {
    auto json = nlohmann::json::parse(message, nullptr, false);
    if (json.is_discarded()) {
        return Result::SUCCESS;
    }

    auto arg_it = json.find("arg");
    if (arg_it == json.end() || !arg_it->is_object()) {
        return Result::SUCCESS;
    }
    auto ch_it = arg_it->find("channel");
    if (ch_it == arg_it->end() || !ch_it->is_string() || ch_it->get<std::string>() != "books") {
        return Result::SUCCESS;
    }

    auto inst_it = arg_it->find("instId");
    if (inst_it == arg_it->end() || !inst_it->is_string() ||
        inst_it->get<std::string>() != instrument_id_) {
        return Result::SUCCESS;
    }

    auto action_it = json.find("action");
    if (action_it != json.end() && action_it->is_string() &&
        action_it->get<std::string>() == "snapshot") {
        return process_snapshot(json);
    }

    auto data_it = json.find("data");
    if (data_it == json.end() || !data_it->is_array() || data_it->empty()) {
        return Result::SUCCESS;
    }
    const auto* data_src = &(*data_it)[0];

    uint64_t seq = 0;
    uint64_t prev_seq = 0;

    auto seq_it = data_src->find("seqId");
    if (seq_it != data_src->end()) {
        if (seq_it->is_string()) {
            seq = std::stoull(seq_it->get<std::string>());
        } else {
            seq = seq_it->get<uint64_t>();
        }
    }

    auto prev_it = data_src->find("prevSeqId");
    if (prev_it != data_src->end()) {
        if (prev_it->is_string()) {
            prev_seq = std::stoull(prev_it->get<std::string>());
        } else {
            prev_seq = prev_it->get<uint64_t>();
        }
    }

    auto cur = state_.load(std::memory_order_acquire);
    if (cur == State::BUFFERING || cur == State::DISCONNECTED) {
        if (delta_buffer_.size() < MAX_BUFFER_SIZE) {
            delta_buffer_.push_back(message);
        } else {
            trigger_resnapshot("Buffer overflow");
        }
        return Result::SUCCESS;
    }

    if (!validate_delta_sequence(seq, prev_seq)) {
        trigger_resnapshot("OKX sequence gap");
        return Result::ERROR_SEQUENCE_GAP;
    }

    if (!validate_checksum(*data_src)) {
        trigger_resnapshot("OKX checksum mismatch");
        return Result::ERROR_BOOK_CORRUPTED;
    }

    return process_delta(json, seq);
}

auto OkxFeedHandler::apply_buffered_deltas() -> Result {
    size_t applied = 0;
    size_t skipped = 0;

    for (const auto& msg : delta_buffer_) {
        auto json = nlohmann::json::parse(msg, nullptr, false);
        if (json.is_discarded()) {
            continue;
        }

        auto data_it = json.find("data");
        if (data_it == json.end() || !data_it->is_array() || data_it->empty()) {
            continue;
        }

        const auto& data = (*data_it)[0];
        uint64_t seq = 0;
        uint64_t prev_seq = 0;

        auto seq_it = data.find("seqId");
        if (seq_it != data.end()) {
            if (seq_it->is_string()) {
                seq = std::stoull(seq_it->get<std::string>());
            } else {
                seq = seq_it->get<uint64_t>();
            }
        }

        auto prev_it = data.find("prevSeqId");
        if (prev_it != data.end()) {
            if (prev_it->is_string()) {
                prev_seq = std::stoull(prev_it->get<std::string>());
            } else {
                prev_seq = prev_it->get<uint64_t>();
            }
        }

        if (seq <= last_sequence_.load(std::memory_order_acquire)) {
            ++skipped;
            continue;
        }

        if (!validate_delta_sequence(seq, prev_seq)) {
            delta_buffer_.clear();
            return Result::ERROR_SEQUENCE_GAP;
        }

        if (!validate_checksum(data)) {
            delta_buffer_.clear();
            return Result::ERROR_BOOK_CORRUPTED;
        }

        if (process_delta(json, seq) == Result::SUCCESS) {
            ++applied;
        }
    }

    delta_buffer_.clear();
    LOG_INFO("[OKX] Applied buffered deltas", "applied", applied, "skipped", skipped);
    return Result::SUCCESS;
}

auto OkxFeedHandler::validate_delta_sequence(uint64_t seq, uint64_t prev_seq) const -> bool {
    uint64_t last = last_sequence_.load(std::memory_order_acquire);
    if (prev_seq != 0) {
        return prev_seq == last;
    }
    return seq > last;
}

auto OkxFeedHandler::validate_checksum(const nlohmann::json& data) const -> bool {
    auto checksum_it = data.find("checksum");
    if (checksum_it == data.end()) {
        return true;
    }

    int64_t remote_checksum = 0;
    if (checksum_it->is_string()) {
        remote_checksum = std::stoll(checksum_it->get<std::string>());
    } else {
        remote_checksum = checksum_it->get<int64_t>();
    }

    auto test_bids = bids_;
    auto test_asks = asks_;

    auto apply_side = [](const nlohmann::json& arr, auto& book_side) {
        if (!arr.is_array()) {
            return;
        }
        for (const auto& lvl : arr) {
            if (!lvl.is_array() || lvl.size() < 2) {
                continue;
            }
            const double price = std::stod(lvl[0].get<std::string>());
            const std::string price_str = lvl[0].get<std::string>();
            const std::string size_str = lvl[1].get<std::string>();
            if (size_str == "0" || size_str == "0.0" || std::stod(size_str) == 0.0) {
                book_side.erase(price);
            } else {
                book_side[price] = {price_str, size_str};
            }
        }
    };

    apply_side(data.value("bids", nlohmann::json::array()), test_bids);
    apply_side(data.value("asks", nlohmann::json::array()), test_asks);

    std::ostringstream oss;
    auto bid_it = test_bids.begin();
    auto ask_it = test_asks.begin();
    for (size_t i = 0; i < 25 && (bid_it != test_bids.end() || ask_it != test_asks.end()); ++i) {
        if (i > 0) {
            oss << ':';
        }
        if (bid_it != test_bids.end()) {
            oss << bid_it->second.first << ':' << bid_it->second.second;
            ++bid_it;
        }
        if (ask_it != test_asks.end()) {
            if (oss.tellp() > 0) {
                oss << ':';
            }
            oss << ask_it->second.first << ':' << ask_it->second.second;
            ++ask_it;
        }
    }

    const auto serialized = oss.str();
    const uint32_t local = feed::crc32_bytes(serialized);
    const int64_t local_signed = static_cast<int32_t>(local);

    if (local_signed != remote_checksum) {
        LOG_ERROR("[OKX] checksum mismatch", "symbol", symbol_.c_str(), "local", local_signed,
                  "remote", remote_checksum);
        return false;
    }

    return true;
}

void OkxFeedHandler::apply_local_book_levels(const nlohmann::json& data) {
    auto apply_side = [](const nlohmann::json& arr, auto& book_side) {
        if (!arr.is_array()) {
            return;
        }
        for (const auto& lvl : arr) {
            if (!lvl.is_array() || lvl.size() < 2) {
                continue;
            }
            const double price = std::stod(lvl[0].get<std::string>());
            const std::string price_str = lvl[0].get<std::string>();
            const std::string size_str = lvl[1].get<std::string>();

            if (size_str == "0" || size_str == "0.0" || std::stod(size_str) == 0.0) {
                book_side.erase(price);
            } else {
                book_side[price] = {price_str, size_str};
            }
        }
    };

    apply_side(data.value("bids", nlohmann::json::array()), bids_);
    apply_side(data.value("asks", nlohmann::json::array()), asks_);
}

void OkxFeedHandler::trigger_resnapshot(const std::string& reason) {
    LOG_WARN("[OKX] Triggering re-snapshot", "reason", reason.c_str());
    if (error_callback_) {
        error_callback_("Re-snapshot: " + reason);
    }
    delta_buffer_.clear();
    state_.store(State::BUFFERING, std::memory_order_release);
    force_reconnect_.store(true, std::memory_order_release);
    if (auto* ctx = static_cast<struct lws_context*>(lws_ctx_.load(std::memory_order_acquire))) {
        lws_cancel_service(ctx);
    }
}

}
