#include "okx_feed_handler.hpp"
#include "../../common/rest_client.hpp"
#include <libwebsockets.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>

namespace trading {

namespace {

bool parse_decimal(const std::string& text, double& out) {
    const char* begin = text.c_str();
    char* end = nullptr;
    out = std::strtod(begin, &end);
    return end != begin && end && *end == '\0';
}

}  // namespace

struct OkxWsSession {
    OkxFeedHandler* handler;
    struct lws* wsi{nullptr};
    bool established{false};
    bool closed{false};
    bool send_ping{false};
    bool send_subscribe{false};
    int64_t last_ping_ns{0};
    std::string subscribe_msg;
    char frag_buf[131072];
    size_t frag_len{0};
};

static int okx_lws_cb(struct lws* wsi,
                      enum lws_callback_reasons reason,
                      void* /*user*/, void* in, size_t len) {
    auto* s = static_cast<OkxWsSession*>(lws_context_user(lws_get_context(wsi)));
    if (!s) return 0;

    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        s->wsi = wsi;
        s->established = true;
        s->send_subscribe = true;
        s->last_ping_ns = http::now_ns();
        lws_callback_on_writable(wsi);
        lws_cancel_service(lws_get_context(wsi));
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        const char* data = static_cast<const char*>(in);
        const bool is_final = lws_is_final_fragment(wsi);
        if (s->frag_len + len < sizeof(s->frag_buf)) {
            std::memcpy(s->frag_buf + s->frag_len, data, len);
            s->frag_len += len;
        }
        if (is_final && s->handler) {
            s->handler->process_message(std::string(s->frag_buf, s->frag_len));
            s->frag_len = 0;
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        if (s->send_subscribe && !s->subscribe_msg.empty()) {
            std::vector<unsigned char> buf(LWS_PRE + s->subscribe_msg.size());
            std::memcpy(buf.data() + LWS_PRE, s->subscribe_msg.c_str(), s->subscribe_msg.size());
            lws_write(wsi, buf.data() + LWS_PRE, s->subscribe_msg.size(), LWS_WRITE_TEXT);
            s->send_subscribe = false;
        } else if (s->send_ping) {
            unsigned char buf[LWS_PRE + 4] = {};
            lws_write(wsi, buf + LWS_PRE, 0, LWS_WRITE_PING);
            s->send_ping = false;
        }
        break;

    case LWS_CALLBACK_CLIENT_CLOSED:
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        s->closed = true;
        lws_cancel_service(lws_get_context(wsi));
        break;

    default:
        break;
    }
    return 0;
}

static struct lws_protocols k_okx_protocols[] = {
    {"okx-books", okx_lws_cb, 0, 131072, 0, nullptr, 0},
    {nullptr, nullptr, 0, 0, 0, nullptr, 0},
};

OkxFeedHandler::OkxFeedHandler(const std::string& symbol,
                               const std::string& api_url,
                               const std::string& ws_url)
    : symbol_(symbol), api_url_(api_url), ws_url_(ws_url) {
    LOG_INFO("OkxFeedHandler created", "symbol", symbol_.c_str());
}

OkxFeedHandler::~OkxFeedHandler() { stop(); }

Result OkxFeedHandler::start() {
    if (running_.load(std::memory_order_acquire)) return Result::SUCCESS;

    running_.store(true, std::memory_order_release);
    state_.store(State::BUFFERING, std::memory_order_release);
    last_sequence_.store(0, std::memory_order_release);
    delta_buffer_.clear();

    ws_thread_ = std::thread([this]() { ws_event_loop(); });

    std::unique_lock<std::mutex> lk(ws_mutex_);
    const bool ready = ws_cv_.wait_for(lk, std::chrono::seconds(30), [this]() {
        return state_.load(std::memory_order_acquire) == State::STREAMING ||
               !running_.load(std::memory_order_acquire);
    });

    if (!ready || !running_.load(std::memory_order_acquire)) {
        running_.store(false, std::memory_order_release);
        if (auto* ctx = static_cast<struct lws_context*>(lws_ctx_.load(std::memory_order_acquire))) {
            lws_cancel_service(ctx);
        }
        ws_cv_.notify_all();
        if (ws_thread_.joinable()) ws_thread_.join();
        return Result::ERROR_CONNECTION_LOST;
    }

    return Result::SUCCESS;
}

void OkxFeedHandler::stop() {
    if (!running_.load(std::memory_order_acquire) && !ws_thread_.joinable()) return;

    running_.store(false, std::memory_order_release);
    state_.store(State::DISCONNECTED, std::memory_order_release);

    if (auto* ctx = static_cast<struct lws_context*>(lws_ctx_.load(std::memory_order_acquire))) {
        lws_cancel_service(ctx);
    }
    ws_cv_.notify_all();

    if (ws_thread_.joinable()) ws_thread_.join();
}

void OkxFeedHandler::ws_event_loop() {
    constexpr uint32_t MAX_DELAY_MS = 30000;
    uint32_t delay_ms = 100;

    std::string ws_host = "ws.okx.com";
    int ws_port = 8443;
    std::string ws_path = "/ws/v5/public";

    {
        size_t pos = ws_url_.find("://");
        pos = (pos == std::string::npos) ? 0 : pos + 3;

        const size_t slash = ws_url_.find('/', pos);
        const std::string authority = ws_url_.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (slash != std::string::npos) ws_path = ws_url_.substr(slash);

        const size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            ws_host = authority.substr(0, colon);
            ws_port = std::stoi(authority.substr(colon + 1));
        } else if (!authority.empty()) {
            ws_host = authority;
        }
    }

    while (running_.load(std::memory_order_acquire)) {
        OkxWsSession session;
        session.handler = this;

        nlohmann::json sub = {
            {"op", "subscribe"},
            {"args", nlohmann::json::array({{{"channel", "books"}, {"instId", symbol_}}})},
        };
        session.subscribe_msg = sub.dump();

        lws_context_creation_info ctx_info = {};
        ctx_info.port = CONTEXT_PORT_NO_LISTEN;
        ctx_info.protocols = k_okx_protocols;
        ctx_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        ctx_info.user = &session;

        auto* ctx = lws_create_context(&ctx_info);
        if (!ctx) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }
        lws_ctx_.store(ctx, std::memory_order_release);

        lws_client_connect_info cc = {};
        cc.context = ctx;
        cc.address = ws_host.c_str();
        cc.port = ws_port;
        cc.path = ws_path.c_str();
        cc.host = ws_host.c_str();
        cc.origin = ws_host.c_str();
        cc.protocol = k_okx_protocols[0].name;
        cc.ssl_connection = LCCSCF_USE_SSL;

        if (!lws_client_connect_via_info(&cc)) {
            lws_context_destroy(ctx);
            lws_ctx_.store(nullptr, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }

        while (running_.load() && !session.established && !session.closed) lws_service(ctx, 50);

        if (!running_.load() || session.closed) {
            lws_context_destroy(ctx);
            lws_ctx_.store(nullptr, std::memory_order_release);
            if (running_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            }
            continue;
        }

        state_.store(State::BUFFERING, std::memory_order_release);
        delta_buffer_.clear();

        if (fetch_snapshot() != Result::SUCCESS || apply_buffered_deltas() != Result::SUCCESS) {
            trigger_resnapshot("Failed snapshot sync");
            lws_context_destroy(ctx);
            lws_ctx_.store(nullptr, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }

        state_.store(State::STREAMING, std::memory_order_release);
        ws_cv_.notify_all();
        delay_ms = 100;

        while (running_.load() && !session.closed) {
            const int64_t now_ns = http::now_ns();
            if (now_ns - session.last_ping_ns >= 20LL * 1'000'000'000LL) {
                session.send_ping = true;
                session.last_ping_ns = now_ns;
                lws_callback_on_writable(session.wsi);
            }
            lws_service(ctx, 50);
        }

        lws_context_destroy(ctx);
        lws_ctx_.store(nullptr, std::memory_order_release);
        state_.store(State::BUFFERING, std::memory_order_release);

        if (running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
        }
    }

    ws_cv_.notify_all();
}

bool OkxFeedHandler::parse_seq_pair(const nlohmann::json& data, uint64_t& seq, uint64_t& prev_seq) const {
    seq = 0;
    prev_seq = 0;

    auto seq_it = data.find("seqId");
    if (seq_it == data.end()) return false;
    if (seq_it->is_string()) {
        seq = std::strtoull(seq_it->get<std::string>().c_str(), nullptr, 10);
    } else if (seq_it->is_number_unsigned()) {
        seq = seq_it->get<uint64_t>();
    }

    auto prev_it = data.find("prevSeqId");
    if (prev_it != data.end()) {
        if (prev_it->is_string()) {
            prev_seq = std::strtoull(prev_it->get<std::string>().c_str(), nullptr, 10);
        } else if (prev_it->is_number_unsigned()) {
            prev_seq = prev_it->get<uint64_t>();
        }
    }

    return seq > 0;
}

bool OkxFeedHandler::apply_levels(const nlohmann::json& levels, bool is_bid) {
    if (!levels.is_array()) return false;

    bool changed = false;
    for (const auto& lvl : levels) {
        if (!lvl.is_array() || lvl.size() < 2 || !lvl[0].is_string() || !lvl[1].is_string()) continue;

        double px = 0.0, qty = 0.0;
        if (!parse_decimal(lvl[0].get<std::string>(), px) || !parse_decimal(lvl[1].get<std::string>(), qty)) continue;

        if (is_bid) {
            if (qty <= 0.0) {
                changed = changed || (bids_.erase(px) > 0);
            } else {
                bids_[px] = qty;
                changed = true;
            }
        } else {
            if (qty <= 0.0) {
                changed = changed || (asks_.erase(px) > 0);
            } else {
                asks_[px] = qty;
                changed = true;
            }
        }
    }
    return changed;
}

uint32_t OkxFeedHandler::crc32_bytes(const unsigned char* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint32_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

uint32_t OkxFeedHandler::compute_checksum() const {
    std::string payload;
    payload.reserve(2048);

    auto bid_it = bids_.begin();
    auto ask_it = asks_.begin();

    for (int i = 0; i < 25; ++i) {
        const bool has_bid = bid_it != bids_.end();
        const bool has_ask = ask_it != asks_.end();
        if (!has_bid && !has_ask) break;

        if (has_bid) {
            payload += std::to_string(bid_it->first);
            payload += ':';
            payload += std::to_string(bid_it->second);
            ++bid_it;
        }
        if (has_ask) {
            if (!payload.empty()) payload += ':';
            payload += std::to_string(ask_it->first);
            payload += ':';
            payload += std::to_string(ask_it->second);
            ++ask_it;
        }
        if (i != 24 && (bid_it != bids_.end() || ask_it != asks_.end())) payload += ':';
    }

    return crc32_bytes(reinterpret_cast<const unsigned char*>(payload.data()), payload.size());
}

Result OkxFeedHandler::fetch_snapshot() {
    const std::string url = api_url_ + "/api/v5/market/books?instId=" + symbol_ + "&sz=400";
    const auto resp = http::get(url);
    if (resp.body.empty()) return Result::ERROR_CONNECTION_LOST;

    const auto j = nlohmann::json::parse(resp.body, nullptr, false);
    if (j.is_discarded()) return Result::ERROR_BOOK_CORRUPTED;

    const auto code_it = j.find("code");
    if (code_it == j.end() || !code_it->is_string() || code_it->get<std::string>() != "0") return Result::ERROR_BOOK_CORRUPTED;

    const auto data_it = j.find("data");
    if (data_it == j.end() || !data_it->is_array() || data_it->empty()) return Result::ERROR_BOOK_CORRUPTED;

    const auto& book = (*data_it)[0];

    Snapshot snap;
    snap.symbol = symbol_;
    snap.exchange = Exchange::OKX;
    snap.timestamp_local_ns = http::now_ns();

    uint64_t seq = 0, prev_seq = 0;
    if (!parse_seq_pair(book, seq, prev_seq)) return Result::ERROR_INVALID_SEQUENCE;
    snap.sequence = seq;

    bids_.clear();
    asks_.clear();
    apply_levels(book.value("bids", nlohmann::json::array()), true);
    apply_levels(book.value("asks", nlohmann::json::array()), false);

    for (const auto& [px, qty] : bids_) snap.bids.push_back({px, qty});
    for (const auto& [px, qty] : asks_) snap.asks.push_back({px, qty});

    if (snap.bids.empty() || snap.asks.empty()) return Result::ERROR_BOOK_CORRUPTED;

    last_sequence_.store(seq, std::memory_order_release);
    if (snapshot_callback_) snapshot_callback_(snap);
    return Result::SUCCESS;
}

Result OkxFeedHandler::process_delta(const nlohmann::json& j, uint64_t seq) {
    const auto data_it = j.find("data");
    if (data_it == j.end() || !data_it->is_array() || data_it->empty()) return Result::SUCCESS;
    const auto& book = (*data_it)[0];

    apply_levels(book.value("bids", nlohmann::json::array()), true);
    apply_levels(book.value("asks", nlohmann::json::array()), false);

    auto checksum_it = book.find("checksum");
    if (checksum_it != book.end()) {
        int32_t wire = 0;
        if (checksum_it->is_number_integer()) {
            wire = checksum_it->get<int32_t>();
        } else if (checksum_it->is_string()) {
            wire = static_cast<int32_t>(std::strtol(checksum_it->get<std::string>().c_str(), nullptr, 10));
        }

        const int32_t local = static_cast<int32_t>(compute_checksum());
        if (local != wire) {
            LOG_ERROR("OKX checksum mismatch", "local", local, "wire", wire);
            return Result::ERROR_BOOK_CORRUPTED;
        }
    }

    last_sequence_.store(seq, std::memory_order_release);
    const int64_t ts = http::now_ns();

    for (const auto& lvl : book.value("bids", nlohmann::json::array())) {
        if (!lvl.is_array() || lvl.size() < 2 || !lvl[0].is_string() || !lvl[1].is_string()) continue;
        double px = 0.0, qty = 0.0;
        if (!parse_decimal(lvl[0].get<std::string>(), px) || !parse_decimal(lvl[1].get<std::string>(), qty)) continue;
        Delta d;
        d.side = Side::BID;
        d.price = px;
        d.size = qty;
        d.sequence = seq;
        d.timestamp_local_ns = ts;
        if (delta_callback_) delta_callback_(d);
    }

    for (const auto& lvl : book.value("asks", nlohmann::json::array())) {
        if (!lvl.is_array() || lvl.size() < 2 || !lvl[0].is_string() || !lvl[1].is_string()) continue;
        double px = 0.0, qty = 0.0;
        if (!parse_decimal(lvl[0].get<std::string>(), px) || !parse_decimal(lvl[1].get<std::string>(), qty)) continue;
        Delta d;
        d.side = Side::ASK;
        d.price = px;
        d.size = qty;
        d.sequence = seq;
        d.timestamp_local_ns = ts;
        if (delta_callback_) delta_callback_(d);
    }

    return Result::SUCCESS;
}

Result OkxFeedHandler::process_message(const std::string& message) {
    const auto j = nlohmann::json::parse(message, nullptr, false);
    if (j.is_discarded()) return Result::SUCCESS;

    const auto arg_it = j.find("arg");
    if (arg_it == j.end() || !arg_it->is_object()) return Result::SUCCESS;

    const auto ch_it = arg_it->find("channel");
    const auto inst_it = arg_it->find("instId");
    if (ch_it == arg_it->end() || !ch_it->is_string() || ch_it->get<std::string>() != "books") return Result::SUCCESS;
    if (inst_it != arg_it->end() && inst_it->is_string() && inst_it->get<std::string>() != symbol_) return Result::SUCCESS;

    const auto data_it = j.find("data");
    if (data_it == j.end() || !data_it->is_array() || data_it->empty()) return Result::SUCCESS;

    uint64_t seq = 0, prev_seq = 0;
    if (!parse_seq_pair((*data_it)[0], seq, prev_seq)) return Result::ERROR_INVALID_SEQUENCE;

    const auto cur = state_.load(std::memory_order_acquire);
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

    const Result r = process_delta(j, seq);
    if (r == Result::ERROR_BOOK_CORRUPTED) {
        trigger_resnapshot("OKX checksum mismatch");
    }
    return r;
}

Result OkxFeedHandler::apply_buffered_deltas() {
    size_t applied = 0;
    size_t skipped = 0;

    for (const auto& msg : delta_buffer_) {
        const auto j = nlohmann::json::parse(msg, nullptr, false);
        if (j.is_discarded()) continue;

        const auto data_it = j.find("data");
        if (data_it == j.end() || !data_it->is_array() || data_it->empty()) continue;

        uint64_t seq = 0, prev_seq = 0;
        if (!parse_seq_pair((*data_it)[0], seq, prev_seq)) continue;

        const uint64_t last = last_sequence_.load(std::memory_order_acquire);
        if (seq <= last) {
            ++skipped;
            continue;
        }

        if (!validate_delta_sequence(seq, prev_seq)) {
            delta_buffer_.clear();
            return Result::ERROR_SEQUENCE_GAP;
        }

        const Result r = process_delta(j, seq);
        if (r != Result::SUCCESS) {
            delta_buffer_.clear();
            return r;
        }
        ++applied;
    }

    delta_buffer_.clear();
    LOG_INFO("Applied OKX buffered deltas", "applied", applied, "skipped", skipped);
    return Result::SUCCESS;
}

bool OkxFeedHandler::validate_delta_sequence(uint64_t seq, uint64_t prev_seq) const {
    const uint64_t last = last_sequence_.load(std::memory_order_acquire);
    if (prev_seq != 0) return prev_seq == last && seq > last;
    return seq == last + 1;
}

void OkxFeedHandler::trigger_resnapshot(const std::string& reason) {
    LOG_WARN("Triggering OKX re-snapshot", "reason", reason.c_str());
    if (error_callback_) error_callback_("Re-snapshot: " + reason);
    delta_buffer_.clear();
    state_.store(State::BUFFERING, std::memory_order_release);
}

}  // namespace trading
