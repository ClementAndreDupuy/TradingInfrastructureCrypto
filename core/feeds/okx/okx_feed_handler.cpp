#include "okx_feed_handler.hpp"
#include "../../common/rest_client.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <libwebsockets.h>
#include <sstream>

namespace trading {

static auto crc32_bytes(const std::string& data) -> uint32_t {
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char byte : data) {
        crc ^= static_cast<uint32_t>(byte);
        for (int i = 0; i < 8; ++i) {
            uint32_t mask = static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1u)));
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

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

static auto okx_lws_cb(struct lws* wsi, enum lws_callback_reasons reason, void* /*user*/,
                       void* input, size_t len) -> int {
    auto* session = static_cast<OkxWsSession*>(lws_context_user(lws_get_context(wsi)));
    if (session == nullptr) {
        return 0;
    }

    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        session->wsi = wsi;
        session->established = true;
        session->send_subscribe = true;
        session->last_ping_ns = http::now_ns();
        lws_callback_on_writable(wsi);
        lws_cancel_service(lws_get_context(wsi));
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        const char* data = static_cast<const char*>(input);
        bool is_final = lws_is_final_fragment(wsi) != 0;
        if (session->frag_len + len <= sizeof(session->frag_buf)) {
            std::memcpy(session->frag_buf + session->frag_len, data, len);
            session->frag_len += len;
        } else {
            // Message exceeds buffer: discard the entire fragmented message.
            session->frag_len = 0;
        }
        if (is_final && session->frag_len > 0 && (session->handler != nullptr)) {
            session->handler->process_message(std::string(session->frag_buf, session->frag_len));
            session->frag_len = 0;
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        if (session->send_subscribe && !session->subscribe_msg.empty()) {
            std::vector<unsigned char> buf(LWS_PRE + session->subscribe_msg.size());
            std::memcpy(buf.data() + LWS_PRE, session->subscribe_msg.c_str(),
                        session->subscribe_msg.size());
            lws_write(wsi, buf.data() + LWS_PRE, session->subscribe_msg.size(), LWS_WRITE_TEXT);
            session->send_subscribe = false;
        } else if (session->send_ping) {
            unsigned char buf[LWS_PRE + 4] = {};
            lws_write(wsi, buf + LWS_PRE, 0, LWS_WRITE_PING);
            session->send_ping = false;
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_CLOSED:
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        session->closed = true;
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

OkxFeedHandler::OkxFeedHandler(const std::string& symbol, const std::string& api_url,
                               const std::string& ws_url)
    : symbol_(symbol), venue_symbols_(SymbolMapper::map_all(symbol)),
      inst_id_(venue_symbols_.okx), api_url_(api_url), ws_url_(ws_url) {
    LOG_INFO("OkxFeedHandler created", "symbol", symbol_.c_str(), "inst_id",
             inst_id_.c_str());
}

OkxFeedHandler::~OkxFeedHandler() { stop(); }

auto OkxFeedHandler::start() -> Result {
    if (running_.load(std::memory_order_acquire)) {
        return Result::SUCCESS;
    }

    LOG_INFO("Starting OKX feed handler", "symbol", symbol_.c_str());
    running_.store(true, std::memory_order_release);
    state_.store(State::BUFFERING, std::memory_order_release);

    ws_thread_ = std::thread([this]() -> void { ws_event_loop(); });

    std::unique_lock<std::mutex> lock(ws_mutex_);
    bool ready = ws_cv_.wait_for(lock, std::chrono::seconds(30), [this]() -> bool {
        return state_.load(std::memory_order_acquire) == State::STREAMING ||
               !running_.load(std::memory_order_acquire);
    });

    if (!ready || !running_.load(std::memory_order_acquire)) {
        running_.store(false, std::memory_order_release);
        if (auto* ctx =
                static_cast<struct lws_context*>(lws_ctx_.load(std::memory_order_acquire))) {
            lws_cancel_service(ctx);
        }
        ws_cv_.notify_all();
        if (ws_thread_.joinable()) {
            ws_thread_.join();
        }
        return Result::ERROR_CONNECTION_LOST;
    }

    LOG_INFO("OKX feed handler started", "symbol", symbol_.c_str());
    return Result::SUCCESS;
}

void OkxFeedHandler::stop() {
    if (!running_.load(std::memory_order_acquire) && !ws_thread_.joinable()) {
        return;
    }

    LOG_INFO("Stopping OKX feed handler", "symbol", symbol_.c_str());
    running_.store(false, std::memory_order_release);
    state_.store(State::DISCONNECTED, std::memory_order_release);

    if (auto* ctx = static_cast<struct lws_context*>(lws_ctx_.load(std::memory_order_acquire))) {
        lws_cancel_service(ctx);
    }
    ws_cv_.notify_all();

    if (ws_thread_.joinable()) {
        ws_thread_.join();
    }
}

void OkxFeedHandler::ws_event_loop() {
    constexpr uint32_t MAX_DELAY_MS = 30000;
    uint32_t delay_ms = 100;

    std::string ws_host = "ws.okx.com";
    int ws_port = 8443;
    std::string ws_path = "/ws/v5/public";

    {
        size_t pos = ws_url_.find("://");
        if (pos != std::string::npos) {
            pos += 3;
        } else {
            pos = 0;
        }

        size_t slash = ws_url_.find('/', pos);
        std::string authority =
            ws_url_.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (slash != std::string::npos) {
            ws_path = ws_url_.substr(slash);
        }

        size_t colon = authority.rfind(':');
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
            {"args", nlohmann::json::array({{{"channel", "books"}, {"instId", inst_id_}}})},
        };
        session.subscribe_msg = sub.dump();

        lws_context_creation_info ctx_info = {};
        ctx_info.port = CONTEXT_PORT_NO_LISTEN;
        ctx_info.protocols = k_okx_protocols;
        ctx_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        ctx_info.user = &session;

        auto* ctx = lws_create_context(&ctx_info);
        if (ctx == nullptr) {
            LOG_ERROR("lws_create_context failed", "symbol", symbol_.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }
        lws_ctx_.store(ctx, std::memory_order_release);

        lws_client_connect_info connect_info = {};
        connect_info.context = ctx;
        connect_info.address = ws_host.c_str();
        connect_info.port = ws_port;
        connect_info.path = ws_path.c_str();
        connect_info.host = ws_host.c_str();
        connect_info.origin = ws_host.c_str();
        connect_info.protocol = k_okx_protocols[0].name;
        connect_info.ssl_connection = LCCSCF_USE_SSL;

        if (lws_client_connect_via_info(&connect_info) == nullptr) {
            LOG_ERROR("OKX WebSocket connect failed", "symbol", symbol_.c_str());
            lws_context_destroy(ctx);
            lws_ctx_.store(nullptr, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }

        while (running_.load() && !session.established && !session.closed) {
            lws_service(ctx, 50);
        }

        if (!running_.load() || session.closed) {
            lws_context_destroy(ctx);
            lws_ctx_.store(nullptr, std::memory_order_release);
            if (running_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            }
            continue;
        }

        LOG_INFO("OKX WebSocket established", "symbol", symbol_.c_str());

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
            int64_t now_ns = http::now_ns();
            if (now_ns - session.last_ping_ns >= 20LL * 1'000'000'000LL) {
                session.send_ping = true;
                session.last_ping_ns = now_ns;
                lws_callback_on_writable(session.wsi);
            }
            lws_service(ctx, 50);
        }

        lws_context_destroy(ctx);
        lws_ctx_.store(nullptr, std::memory_order_release);

        if (running_.load(std::memory_order_acquire)) {
            LOG_WARN("OKX stream disconnected, reconnecting", "symbol", symbol_.c_str(),
                     "backoff_ms", delay_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
        }
    }

    ws_cv_.notify_all();
}

auto OkxFeedHandler::fetch_snapshot() -> Result {
    const std::string url = api_url_ + "/api/v5/market/books?instId=" + inst_id_ + "&sz=400";

    auto resp = http::get(url);
    if (resp.body.empty()) {
        return Result::ERROR_CONNECTION_LOST;
    }

    auto json = nlohmann::json::parse(resp.body, nullptr, false);
    if (json.is_discarded()) {
        return Result::ERROR_BOOK_CORRUPTED;
    }

    auto code_it = json.find("code");
    if (code_it == json.end() || code_it->get<std::string>() != "0") {
        return Result::ERROR_BOOK_CORRUPTED;
    }

    auto data_it = json.find("data");
    if (data_it == json.end() || !data_it->is_array() || data_it->empty()) {
        return Result::ERROR_BOOK_CORRUPTED;
    }

    const auto& book = (*data_it)[0];

    Snapshot snap;
    snap.symbol = symbol_;
    snap.exchange = Exchange::OKX;
    snap.timestamp_local_ns = http::now_ns();

    auto seq_it = book.find("seqId");
    if (seq_it != book.end()) {
        if (seq_it->is_string()) {
            snap.sequence = std::stoull(seq_it->get<std::string>());
        } else {
            snap.sequence = seq_it->get<uint64_t>();
        }
    }

    const char* symbol = symbol_.c_str();
    auto parse_levels = [symbol](const nlohmann::json& arr) -> std::vector<PriceLevel> {
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
                LOG_WARN("OKX snapshot level parse failed", "symbol", symbol, "error", ex.what());
            }
        }
        return out;
    };

    snap.bids = parse_levels(book.value("bids", nlohmann::json::array()));
    snap.asks = parse_levels(book.value("asks", nlohmann::json::array()));

    if (snap.bids.empty() || snap.asks.empty()) {
        return Result::ERROR_BOOK_CORRUPTED;
    }

    bids_.clear();
    asks_.clear();
    apply_local_book_levels(book);

    last_sequence_.store(snap.sequence, std::memory_order_release);
    if (snapshot_callback_) {
        snapshot_callback_(snap);
    }

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
    if (inst_it == arg_it->end() || !inst_it->is_string() || inst_it->get<std::string>() != inst_id_) {
        return Result::SUCCESS;
    }

    const nlohmann::json* data_src = nullptr;
    auto data_it = json.find("data");
    if (data_it == json.end() || !data_it->is_array() || data_it->empty()) {
        return Result::SUCCESS;
    }
    data_src = &(*data_it)[0];

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
    LOG_INFO("Applied OKX buffered deltas", "applied", applied, "skipped", skipped);
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
    auto apply_side = [](const nlohmann::json& arr, auto& book_side) -> auto {
        if (!arr.is_array()) {
            return;
        }
        for (const auto& lvl : arr) {
            if (!lvl.is_array() || lvl.size() < 2) {
                continue;
            }
            const double price = std::stod(lvl[0].get<std::string>());
            const std::string size = lvl[1].get<std::string>();
            if (size == "0" || size == "0.0" || std::stod(size) == 0.0) {
                book_side.erase(price);
            } else {
                book_side[price] = size;
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
            oss << std::setprecision(15) << bid_it->first << ':' << bid_it->second;
            ++bid_it;
        }
        if (ask_it != test_asks.end()) {
            if (oss.tellp() > 0) {
                oss << ':';
            }
            oss << std::setprecision(15) << ask_it->first << ':' << ask_it->second;
            ++ask_it;
        }
    }

    const auto serialized = oss.str();
    const uint32_t local = crc32_bytes(serialized);
    const int64_t local_signed = static_cast<int32_t>(local);

    if (local_signed != remote_checksum) {
        LOG_ERROR("OKX checksum mismatch", "symbol", symbol_.c_str(), "local", local_signed,
                  "remote", remote_checksum);
        return false;
    }

    return true;
}

void OkxFeedHandler::apply_local_book_levels(const nlohmann::json& data) {
    auto apply_side = [](const nlohmann::json& arr, auto& book_side) -> auto {
        if (!arr.is_array()) {
            return;
        }
        for (const auto& lvl : arr) {
            if (!lvl.is_array() || lvl.size() < 2) {
                continue;
            }
            double price = std::stod(lvl[0].get<std::string>());
            std::string size = lvl[1].get<std::string>();

            if (size == "0" || size == "0.0" || std::stod(size) == 0.0) {
                book_side.erase(price);
            } else {
                book_side[price] = size;
            }
        }
    };

    apply_side(data.value("bids", nlohmann::json::array()), bids_);
    apply_side(data.value("asks", nlohmann::json::array()), asks_);
}

void OkxFeedHandler::trigger_resnapshot(const std::string& reason) {
    LOG_WARN("Triggering OKX re-snapshot", "reason", reason.c_str());
    if (error_callback_) {
        error_callback_("Re-snapshot: " + reason);
    }
    delta_buffer_.clear();
    state_.store(State::BUFFERING, std::memory_order_release);
}

} // namespace trading
