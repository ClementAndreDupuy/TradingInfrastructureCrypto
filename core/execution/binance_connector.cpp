#include "binance_connector.hpp"
#include <chrono>
#include <regex>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <openssl/hmac.h>
#include <openssl/evp.h>

namespace trading {

BinanceConnector::BinanceConnector(
    const std::string& api_key,
    const std::string& api_secret,
    const std::string& api_url,
    const std::string& ws_url)
    : api_key_(api_key.empty() ? get_api_key_from_env() : api_key),
      api_secret_(api_secret.empty() ? get_api_secret_from_env() : api_secret),
      api_url_(api_url),
      ws_url_(ws_url) {}

BinanceConnector::~BinanceConnector() {
    disconnect();
}

std::string BinanceConnector::get_api_key_from_env() {
    const char* v = std::getenv("BINANCE_API_KEY");
    return v ? v : "";
}

std::string BinanceConnector::get_api_secret_from_env() {
    const char* v = std::getenv("BINANCE_API_SECRET");
    return v ? v : "";
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

ConnectorResult BinanceConnector::connect() {
    if (api_key_.empty() || api_secret_.empty()) {
        emit_error("BinanceConnector: missing API credentials");
        return ConnectorResult::ERROR_AUTH;
    }

    auto result = get_listen_key();
    if (result != ConnectorResult::OK) return result;

    // Caller's event loop connects to: ws_url_ + "/" + listen_key_
    connected_.store(true, std::memory_order_release);
    last_keepalive_ns_.store(now_ns(), std::memory_order_release);

    LOG_INFO("BinanceConnector connected",
             "ws_endpoint", (ws_url_ + "/" + listen_key_.substr(0, 8) + "...").c_str());
    return ConnectorResult::OK;
}

void BinanceConnector::disconnect() {
    if (!connected_.load(std::memory_order_acquire)) return;
    connected_.store(false, std::memory_order_release);
    listen_key_.clear();
    LOG_INFO("BinanceConnector disconnected", "state", "disconnected");
}

bool BinanceConnector::is_connected() const {
    return connected_.load(std::memory_order_acquire);
}

// ── Order submission ──────────────────────────────────────────────────────────

ConnectorResult BinanceConnector::submit_order(const Order& order) {
    if (now_ns() - last_keepalive_ns_.load(std::memory_order_relaxed) > KEEPALIVE_INTERVAL_NS)
        keepalive_listen_key();

    std::ostringstream params;
    params << "symbol="   << order.symbol
           << "&side="    << (order.side == Side::BID ? "BUY" : "SELL")
           << "&type="    << (order.type == OrderType::LIMIT ? "LIMIT" : "MARKET");

    if (order.type == OrderType::LIMIT) {
        const char* tif = "GTC";
        if (order.tif == TimeInForce::IOC) tif = "IOC";
        else if (order.tif == TimeInForce::FOK) tif = "FOK";
        params << "&timeInForce=" << tif;
    }

    std::ostringstream qty_ss, px_ss;
    qty_ss << std::fixed << std::setprecision(8) << order.quantity;
    px_ss  << std::fixed << std::setprecision(2) << order.price;

    params << "&quantity=" << qty_ss.str();
    if (order.type == OrderType::LIMIT)
        params << "&price=" << px_ss.str();

    params << "&newClientOrderId=" << order.client_order_id
           << "&timestamp="        << timestamp_ms()
           << "&recvWindow=5000";

    std::string url = api_url_ + "/api/v3/order?" + build_signed_params(params.str());
    std::string response = http_post(url, "");

    if (response.empty()) {
        emit_error("BinanceConnector: no response from submit_order");
        return ConnectorResult::ERROR_NETWORK;
    }
    if (response.find("\"code\"") != std::string::npos &&
        response.find("\"msg\"")  != std::string::npos) {
        LOG_ERROR("Binance order rejected", "response", response.c_str());
        emit_error("BinanceConnector: order rejected: " + response);
        return ConnectorResult::ERROR_REJECTED;
    }

    track_order(order.client_order_id, order.symbol);
    LOG_INFO("Binance order submitted", "client_id", order.client_order_id, "symbol", order.symbol);
    return ConnectorResult::OK;
}

ConnectorResult BinanceConnector::cancel_order(uint64_t client_order_id) {
    const char* symbol = find_tracked_symbol(client_order_id);
    if (!symbol) {
        LOG_ERROR("cancel_order: symbol not found for client id", "id", client_order_id);
        return ConnectorResult::ERROR_UNKNOWN;
    }

    std::ostringstream params;
    params << "symbol=" << symbol
           << "&origClientOrderId=" << client_order_id
           << "&timestamp=" << timestamp_ms()
           << "&recvWindow=5000";

    std::string url = api_url_ + "/api/v3/order?" + build_signed_params(params.str());
    std::string response = http_delete(url);

    if (response.empty()) {
        emit_error("BinanceConnector: no response from cancel_order");
        return ConnectorResult::ERROR_NETWORK;
    }

    LOG_INFO("Binance cancel sent", "client_id", client_order_id, "symbol", symbol);
    return ConnectorResult::OK;
}

ConnectorResult BinanceConnector::cancel_all(const char* symbol) {
    std::ostringstream params;
    params << "symbol="    << symbol
           << "&timestamp=" << timestamp_ms()
           << "&recvWindow=5000";

    std::string url = api_url_ + "/api/v3/openOrders?" + build_signed_params(params.str());
    std::string response = http_delete(url);

    if (response.empty()) {
        emit_error("BinanceConnector: no response from cancel_all");
        return ConnectorResult::ERROR_NETWORK;
    }

    LOG_INFO("Binance cancel_all sent", "symbol", symbol);
    return ConnectorResult::OK;
}

ConnectorResult BinanceConnector::reconcile() {
    std::ostringstream params;
    params << "timestamp=" << timestamp_ms() << "&recvWindow=5000";

    std::string url = api_url_ + "/api/v3/openOrders?" + build_signed_params(params.str());
    std::string response = http_get(url);

    if (response.empty()) {
        emit_error("BinanceConnector: no response from reconcile");
        return ConnectorResult::ERROR_NETWORK;
    }

    LOG_INFO("Binance reconcile complete", "status", "ok");
    return ConnectorResult::OK;
}

// ── ListenKey management ──────────────────────────────────────────────────────

ConnectorResult BinanceConnector::get_listen_key() {
    std::string response = http_post(api_url_ + "/api/v3/userDataStream", "");
    if (response.empty()) {
        emit_error("BinanceConnector: failed to obtain listenKey");
        return ConnectorResult::ERROR_NETWORK;
    }

    std::regex re(R"xxx("listenKey"\s*:\s*"([^"]+)")xxx");
    std::smatch m;
    if (!std::regex_search(response, m, re)) {
        emit_error("BinanceConnector: could not parse listenKey");
        return ConnectorResult::ERROR_UNKNOWN;
    }

    listen_key_ = m[1].str();
    LOG_INFO("Binance listenKey obtained", "status", "ok");
    return ConnectorResult::OK;
}

ConnectorResult BinanceConnector::keepalive_listen_key() {
    if (listen_key_.empty()) return ConnectorResult::ERROR_UNKNOWN;
    http_put(api_url_ + "/api/v3/userDataStream", "listenKey=" + listen_key_);
    last_keepalive_ns_.store(now_ns(), std::memory_order_release);
    LOG_DEBUG("Binance listenKey keepalive", "status", "sent");
    return ConnectorResult::OK;
}

// ── WS message processing ─────────────────────────────────────────────────────

void BinanceConnector::process_ws_message(const std::string& message) {
    if (parse_string(message, "e") == "executionReport")
        on_execution_report(message);
}

void BinanceConnector::on_execution_report(const std::string& msg) {
    FillUpdate update;
    update.local_ts_ns    = now_ns();
    update.exchange_ts_ns = parse_int64(msg, "T") * 1'000'000LL; // ms → ns

    // Client order ID
    std::string cid_str = parse_string(msg, "c");
    if (!cid_str.empty()) {
        try { update.client_order_id = std::stoull(cid_str); } catch (...) {}
    }

    // Exchange order ID
    std::string ex_id = std::to_string(parse_int64(msg, "i"));
    std::strncpy(update.exchange_order_id, ex_id.c_str(), 31);

    update.fill_qty              = parse_double(msg, "l"); // last executed qty
    update.fill_price            = parse_double(msg, "L"); // last executed price
    update.cumulative_filled_qty = parse_double(msg, "z");
    // Binance doesn't send avg_price in executionReport; OrderManager computes it.

    std::string status = parse_string(msg, "X");
    update.new_state   = map_order_status(status);

    std::string reason = parse_string(msg, "r");
    if (!reason.empty() && reason != "NONE")
        std::strncpy(update.reject_reason, reason.c_str(), 63);

    LOG_INFO("Binance executionReport",
             "client_id", update.client_order_id,
             "status",    status.c_str(),
             "fill_qty",  update.fill_qty,
             "fill_px",   update.fill_price);

    emit_fill(update);
}

// ── Signing ───────────────────────────────────────────────────────────────────

std::string BinanceConnector::hmac_sha256_hex(const std::string& data) const {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;

    HMAC(EVP_sha256(),
         api_secret_.data(), static_cast<int>(api_secret_.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         digest, &digest_len);

    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digest_len; ++i)
        hex << std::setw(2) << static_cast<int>(digest[i]);
    return hex.str();
}

std::string BinanceConnector::build_signed_params(const std::string& params) const {
    return params + "&signature=" + hmac_sha256_hex(params);
}

// ── Timestamps ────────────────────────────────────────────────────────────────

int64_t BinanceConnector::timestamp_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t BinanceConnector::now_ns() const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

// ── HTTP helpers (curl via popen, matching existing feed handler pattern) ─────

std::string BinanceConnector::http_get(const std::string& url) const {
    std::string cmd = "curl -s -H 'X-MBX-APIKEY: " + api_key_ + "' '" + url + "'";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string result;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    return result;
}

std::string BinanceConnector::http_post(const std::string& url, const std::string& body) const {
    std::string cmd = "curl -s -X POST -H 'X-MBX-APIKEY: " + api_key_ + "'";
    if (!body.empty()) cmd += " -d '" + body + "'";
    cmd += " '" + url + "'";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string result;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    return result;
}

std::string BinanceConnector::http_put(const std::string& url, const std::string& body) const {
    std::string cmd = "curl -s -X PUT -H 'X-MBX-APIKEY: " + api_key_ + "'";
    if (!body.empty()) cmd += " -d '" + body + "'";
    cmd += " '" + url + "'";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string result;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    return result;
}

std::string BinanceConnector::http_delete(const std::string& url) const {
    std::string cmd = "curl -s -X DELETE -H 'X-MBX-APIKEY: " + api_key_ + "' '" + url + "'";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string result;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    return result;
}

// ── Symbol tracking for cancel_order ─────────────────────────────────────────

void BinanceConnector::track_order(uint64_t client_id, const char* symbol) noexcept {
    uint32_t idx = tracked_count_.fetch_add(1, std::memory_order_relaxed);
    if (idx >= MAX_TRACKED) {
        tracked_count_.fetch_sub(1, std::memory_order_relaxed);
        LOG_WARN("Tracked order table full", "max", MAX_TRACKED);
        return;
    }
    tracked_[idx].client_id = client_id;
    std::strncpy(tracked_[idx].symbol, symbol, 15);
    tracked_[idx].symbol[15] = '\0';
}

const char* BinanceConnector::find_tracked_symbol(uint64_t client_id) const noexcept {
    uint32_t count = tracked_count_.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i)
        if (tracked_[i].client_id == client_id) return tracked_[i].symbol;
    return nullptr;
}

// ── JSON helpers ──────────────────────────────────────────────────────────────

std::string BinanceConnector::parse_string(const std::string& json, const std::string& key) const {
    std::regex re("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch m;
    return std::regex_search(json, m, re) ? m[1].str() : "";
}

double BinanceConnector::parse_double(const std::string& json, const std::string& key) const {
    std::regex re("\"" + key + "\"\\s*:\\s*\"?([0-9.eE+\\-]+)\"?");
    std::smatch m;
    if (!std::regex_search(json, m, re)) return 0.0;
    try { return std::stod(m[1].str()); } catch (...) { return 0.0; }
}

int64_t BinanceConnector::parse_int64(const std::string& json, const std::string& key) const {
    std::regex re("\"" + key + "\"\\s*:\\s*([0-9]+)");
    std::smatch m;
    if (!std::regex_search(json, m, re)) return 0;
    try { return std::stoll(m[1].str()); } catch (...) { return 0; }
}

OrderState BinanceConnector::map_order_status(const std::string& status) noexcept {
    if (status == "NEW")              return OrderState::ACTIVE;
    if (status == "PARTIALLY_FILLED") return OrderState::ACTIVE;
    if (status == "FILLED")           return OrderState::FILLED;
    if (status == "CANCELED")         return OrderState::CANCELED;
    if (status == "PENDING_CANCEL")   return OrderState::ACTIVE;
    if (status == "REJECTED")         return OrderState::REJECTED;
    if (status == "EXPIRED")          return OrderState::CANCELED;
    return OrderState::ACTIVE;
}

}  // namespace trading
