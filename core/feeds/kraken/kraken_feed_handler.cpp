#include "kraken_feed_handler.hpp"
#include "../../common/rest_client.hpp"
#include "../common/tick_size.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <exception>
#include <iomanip>
#include <libwebsockets.h>
#include <regex>
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

uint32_t KrakenFeedHandler::crc32_for_test(const std::string& data) { return crc32_bytes(data); }

struct KrakenWsSession {
    KrakenFeedHandler* handler;
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

static auto kraken_lws_cb(struct lws* wsi, enum lws_callback_reasons reason, void*,
                          void* input, size_t len) -> int {
    auto* session = static_cast<KrakenWsSession*>(lws_context_user(lws_get_context(wsi)));
    if (session == nullptr) {
        return 0;
    }

    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        session->wsi = wsi;
        session->established = true;
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

static struct lws_protocols k_kraken_protocols[] = {
    {"kraken-book", kraken_lws_cb, 0, 131072, 0, nullptr, 0},
    {nullptr, nullptr, 0, 0, 0, nullptr, 0}};

KrakenFeedHandler::KrakenFeedHandler(const std::string& symbol, const std::string& api_url,
                                     const std::string& ws_url)
    : symbol_(symbol), api_url_(api_url), ws_url_(ws_url),
      venue_symbols_(SymbolMapper::map_all(symbol)) {
    LOG_INFO("[Kraken] FeedHandler created", "symbol", symbol_.c_str());
}

KrakenFeedHandler::~KrakenFeedHandler() { stop(); }

auto KrakenFeedHandler::fetch_tick_size() -> Result {
    const std::string url = api_url_ + "/0/public/AssetPairs?pair=" + venue_symbols_.kraken_rest;
    auto resp = http::get(url);
    if (resp.body.empty()) {
        LOG_WARN("[Kraken] fetch_tick_size failed", "symbol", symbol_.c_str(), "status", resp.status);
        return Result::ERROR_CONNECTION_LOST;
    }
    auto json = nlohmann::json::parse(resp.body, nullptr, false);
    if (json.is_discarded()) {
        LOG_WARN("[Kraken] fetch_tick_size JSON parse failed", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }
    auto err_it = json.find("error");
    if (err_it != json.end() && err_it->is_array() && !err_it->empty()) {
        LOG_WARN("[Kraken] fetch_tick_size: API error", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }
    auto res_it = json.find("result");
    if (res_it == json.end() || !res_it->is_object() || res_it->empty()) {
        LOG_WARN("[Kraken] fetch_tick_size: no result", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }
    const auto& pair_data = res_it->begin().value();
    auto ts_it = pair_data.find("tick_size");
    if (ts_it != pair_data.end() && ts_it->is_string()) {
        std::string ts = ts_it->get<std::string>();
        if (!ts.empty()) {
            tick_size_ = tick_from_string(ts);
            LOG_INFO("[Kraken] Tick size fetched", "symbol", symbol_.c_str(), "tick_size", tick_size_);
            return Result::SUCCESS;
        }
    }
    auto dec_it = pair_data.find("pair_decimals");
    if (dec_it == pair_data.end() || !dec_it->is_number_integer()) {
        LOG_WARN("[Kraken] fetch_tick_size: tick_size and pair_decimals both missing", "symbol",
                 symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }
    tick_size_ = std::pow(10.0, -dec_it->get<int>());
    LOG_INFO("[Kraken] Tick size fetched", "symbol", symbol_.c_str(), "tick_size", tick_size_);
    return Result::SUCCESS;
}

auto KrakenFeedHandler::start() -> Result {
    if (running_.load(std::memory_order_acquire)) {
        return Result::SUCCESS;
    }

    LOG_INFO("[Kraken] Starting feed handler", "symbol", symbol_.c_str());
    fetch_tick_size();
    running_.store(true, std::memory_order_release);
    state_.store(State::BUFFERING, std::memory_order_release);
    last_sequence_.store(0, std::memory_order_release);
    bids_.clear();
    asks_.clear();
    delta_buffer_.clear();

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

    LOG_INFO("[Kraken] Feed handler started", "symbol", symbol_.c_str());
    return Result::SUCCESS;
}

void KrakenFeedHandler::stop() {
    if (!running_.load(std::memory_order_acquire) && !ws_thread_.joinable()) {
        return;
    }

    LOG_INFO("[Kraken] Stopping feed handler", "symbol", symbol_.c_str());
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

void KrakenFeedHandler::ws_event_loop() {
    constexpr uint32_t MAX_DELAY_MS = 30000;
    uint32_t delay_ms = 100;

    std::string ws_host = "ws.kraken.com";
    int ws_port = 443;
    std::string ws_path = "/v2";
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

    const std::string ws_sym = venue_symbols_.kraken_ws;
    const std::string subscribe_msg =
        R"({"method":"subscribe","params":{"channel":"book","symbol":[")" + ws_sym +
        R"("],"depth":100,"snapshot":true}})";

    KrakenWsSession session;
    session.handler = this;

    lws_context_creation_info ctx_info = {};
    ctx_info.port = CONTEXT_PORT_NO_LISTEN;
    ctx_info.protocols = k_kraken_protocols;
    ctx_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    ctx_info.user = &session;

    auto* ctx = lws_create_context(&ctx_info);
    if (ctx == nullptr) {
        LOG_ERROR("[Kraken] lws_create_context failed", "symbol", symbol_.c_str());
        running_.store(false, std::memory_order_release);
        ws_cv_.notify_all();
        return;
    }
    lws_ctx_.store(ctx, std::memory_order_release);

    while (running_.load(std::memory_order_acquire)) {
        session.wsi = nullptr;
        session.established = false;
        session.closed = false;
        session.send_ping = false;
        session.send_subscribe = true;
        session.last_ping_ns = 0;
        session.subscribe_msg = subscribe_msg;
        session.frag_len = 0;

        lws_client_connect_info connect_info = {};
        connect_info.context = ctx;
        connect_info.address = ws_host.c_str();
        connect_info.port = ws_port;
        connect_info.path = ws_path.c_str();
        connect_info.host = ws_host.c_str();
        connect_info.origin = ws_host.c_str();
        connect_info.protocol = k_kraken_protocols[0].name;
        connect_info.ssl_connection = LCCSCF_USE_SSL;

        if (lws_client_connect_via_info(&connect_info) == nullptr) {
            LOG_ERROR("[Kraken] WebSocket connect failed", "symbol", symbol_.c_str());
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }

        while (running_.load() && !session.established && !session.closed) {
            lws_service(ctx, 50);
        }

        if (!running_.load()) {
            break;
        }
        if (session.closed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }

        LOG_INFO("[Kraken] WebSocket established, waiting for snapshot", "symbol", symbol_.c_str());
        state_.store(State::BUFFERING, std::memory_order_release);
        delta_buffer_.clear();
        bids_.clear();
        asks_.clear();
        last_sequence_.store(0, std::memory_order_release);
        force_reconnect_.store(false, std::memory_order_release);
        delay_ms = 100;

        while (running_.load() && !session.closed && !force_reconnect_.load(std::memory_order_acquire)) {
            lws_service(ctx, 1000);

            if (!session.closed && (session.wsi != nullptr)) {
                int64_t now = http::now_ns();
                if (now - session.last_ping_ns > 20'000'000'000LL) {
                    session.send_ping = true;
                    session.last_ping_ns = now;
                    lws_callback_on_writable(session.wsi);
                }
            }
        }

        force_reconnect_.store(false, std::memory_order_release);

        if (running_.load(std::memory_order_acquire)) {
            LOG_WARN("[Kraken] WebSocket disconnected, reconnecting", "symbol", symbol_.c_str(),
                     "delay_ms", delay_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
        }
    }

    lws_context_destroy(ctx);
    lws_ctx_.store(nullptr, std::memory_order_release);
    ws_cv_.notify_all();
}

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

    return crc32_bytes(payload) == checksum_it->get<uint32_t>();
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
    if (timestamp.empty()) {
        return 0;
    }

    std::tm tm = {};
    std::istringstream ss(timestamp.substr(0, 19));
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) {
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
    const size_t dot = timestamp.find('.');
    const size_t z = timestamp.find('Z');
    if (dot != std::string::npos && z != std::string::npos && z > dot + 1) {
        std::string frac = timestamp.substr(dot + 1, z - dot - 1);
        if (frac.size() > 9) {
            frac.resize(9);
        }
        while (frac.size() < 9) {
            frac.push_back('0');
        }
        nanos = std::stoll(frac);
    }

    return static_cast<int64_t>(seconds) * 1000000000LL + nanos;
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
