#include "kraken_connector.hpp"
#include "../common/rest_client.hpp"
#include <chrono>
#include <regex>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

namespace trading {

KrakenConnector::KrakenConnector(
    const std::string& api_key,
    const std::string& api_secret,
    const std::string& api_url,
    const std::string& ws_url)
    : api_key_(api_key.empty() ? get_api_key_from_env() : api_key),
      api_secret_(api_secret.empty() ? get_api_secret_from_env() : api_secret),
      api_url_(api_url),
      ws_url_(ws_url) {
    next_nonce_.store(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()),
        std::memory_order_relaxed);
}

KrakenConnector::~KrakenConnector() {
    disconnect();
}

std::string KrakenConnector::get_api_key_from_env() { return http::env_var("KRAKEN_API_KEY"); }

std::string KrakenConnector::get_api_secret_from_env() { return http::env_var("KRAKEN_API_SECRET"); }

// ── Lifecycle ─────────────────────────────────────────────────────────────────

ConnectorResult KrakenConnector::connect() {
    if (api_key_.empty() || api_secret_.empty()) {
        emit_error("KrakenConnector: missing API credentials");
        return ConnectorResult::ERROR_AUTH;
    }

    auto result = get_ws_token();
    if (result != ConnectorResult::OK) return result;

    // Caller's event loop:
    //   1. Connect to ws_url_
    //   2. Subscribe: {"method":"subscribe","params":{"channel":"executions","token":"<ws_token_>"}}
    //   3. Feed received messages to process_ws_message()
    connected_.store(true, std::memory_order_release);

    LOG_INFO("KrakenConnector connected", "ws_url", ws_url_.c_str());
    return ConnectorResult::OK;
}

void KrakenConnector::disconnect() {
    if (!connected_.load(std::memory_order_acquire)) return;
    connected_.store(false, std::memory_order_release);
    ws_token_.clear();
    LOG_INFO("KrakenConnector disconnected", "state", "disconnected");
}

bool KrakenConnector::is_connected() const {
    return connected_.load(std::memory_order_acquire);
}

// ── Order submission ──────────────────────────────────────────────────────────

ConnectorResult KrakenConnector::submit_order(const Order& order) {
    std::string nonce = generate_nonce();

    std::string pair = order.symbol; // caller should pass Kraken pair name, e.g. "XBTUSD"

    std::ostringstream body;
    body << "nonce="     << nonce
         << "&ordertype=" << (order.type == OrderType::LIMIT ? "limit" : "market")
         << "&type="      << (order.side == Side::BID ? "buy" : "sell")
         << "&pair="      << pair
         << "&volume="    << std::fixed << std::setprecision(8) << order.quantity
         << "&cl_ord_id=" << order.client_order_id;

    if (order.type == OrderType::LIMIT)
        body << "&price=" << std::fixed << std::setprecision(2) << order.price;

    std::string response = http_post_private("/0/private/AddOrder", body.str());

    if (response.empty()) {
        emit_error("KrakenConnector: no response from AddOrder");
        return ConnectorResult::ERROR_NETWORK;
    }

    // Check for errors
    if (response.find("\"error\":[\"") != std::string::npos) {
        LOG_ERROR("Kraken order rejected", "response", response.c_str());
        emit_error("KrakenConnector: order rejected: " + response);
        return ConnectorResult::ERROR_REJECTED;
    }

    // Extract txid from {"result":{"txid":["OUF4EM-FRGI2-MQMWZD"],...}}
    std::regex txid_re(R"xxx("txid"\s*:\s*\[\s*"([^"]+)")xxx");
    std::smatch m;
    if (std::regex_search(response, m, txid_re)) {
        track_order(order.client_order_id, m[1].str().c_str());
        LOG_INFO("Kraken order submitted",
                 "client_id", order.client_order_id,
                 "txid",      m[1].str().c_str());
    } else {
        LOG_WARN("Kraken: could not parse txid from response", "response", response.c_str());
    }

    return ConnectorResult::OK;
}

ConnectorResult KrakenConnector::cancel_order(uint64_t client_order_id) {
    std::string nonce = generate_nonce();

    // Prefer cancellation by cl_ord_id (which we set at submission time).
    std::ostringstream body;
    body << "nonce=" << nonce << "&cl_ord_id=" << client_order_id;

    std::string response = http_post_private("/0/private/CancelOrder", body.str());

    if (response.empty()) {
        emit_error("KrakenConnector: no response from CancelOrder");
        return ConnectorResult::ERROR_NETWORK;
    }

    LOG_INFO("Kraken cancel sent", "client_id", client_order_id);
    return ConnectorResult::OK;
}

ConnectorResult KrakenConnector::cancel_all(const char* symbol) {
    // Kraken CancelAllOrders cancels every open order (no per-pair filter).
    std::string nonce = generate_nonce();
    std::string body  = "nonce=" + nonce;

    std::string response = http_post_private("/0/private/CancelAllOrders", body);

    if (response.empty()) {
        emit_error("KrakenConnector: no response from CancelAllOrders");
        return ConnectorResult::ERROR_NETWORK;
    }

    LOG_INFO("Kraken cancel_all sent", "symbol", symbol);
    return ConnectorResult::OK;
}

ConnectorResult KrakenConnector::reconcile() {
    std::string nonce = generate_nonce();
    std::string body  = "nonce=" + nonce;

    std::string response = http_post_private("/0/private/OpenOrders", body);

    if (response.empty()) {
        emit_error("KrakenConnector: no response from OpenOrders");
        return ConnectorResult::ERROR_NETWORK;
    }

    LOG_INFO("Kraken reconcile complete", "status", "ok");
    return ConnectorResult::OK;
}

// ── WS token ─────────────────────────────────────────────────────────────────

ConnectorResult KrakenConnector::get_ws_token() {
    std::string nonce    = generate_nonce();
    std::string post_data = "nonce=" + nonce;

    std::string response = http_post_private("/0/private/GetWebSocketsToken", post_data);
    if (response.empty()) {
        emit_error("KrakenConnector: failed to get WS token");
        return ConnectorResult::ERROR_NETWORK;
    }

    // {"error":[],"result":{"token":"..."}}
    std::regex re(R"xxx("token"\s*:\s*"([^"]+)")xxx");
    std::smatch m;
    if (!std::regex_search(response, m, re)) {
        emit_error("KrakenConnector: could not parse WS token");
        return ConnectorResult::ERROR_UNKNOWN;
    }

    ws_token_ = m[1].str();
    LOG_INFO("Kraken WS token obtained", "status", "ok");
    return ConnectorResult::OK;
}

// ── WS message processing ─────────────────────────────────────────────────────

void KrakenConnector::process_ws_message(const std::string& message) {
    if (message.find("\"channel\"") == std::string::npos) return;
    if (parse_string(message, "channel") != "executions") return;
    on_execution_update(message);
}

void KrakenConnector::on_execution_update(const std::string& msg) {
    FillUpdate update;
    update.local_ts_ns = now_ns();

    // Extract cl_ord_id (our client_order_id as string)
    std::string cid_str = parse_string(msg, "cl_ord_id");
    if (!cid_str.empty()) {
        try { update.client_order_id = std::stoull(cid_str); } catch (...) {}
    }

    // Exchange order ID
    std::string order_id = parse_string(msg, "order_id");
    std::strncpy(update.exchange_order_id, order_id.c_str(), 31);
    if (update.exchange_order_id[0] != '\0' && update.client_order_id != 0)
        track_order(update.client_order_id, update.exchange_order_id);

    update.fill_qty              = parse_double(msg, "last_qty");
    update.fill_price            = parse_double(msg, "last_price");
    update.cumulative_filled_qty = parse_double(msg, "qty_filled");
    update.avg_fill_price        = parse_double(msg, "avg_price");

    std::string order_status = parse_string(msg, "order_status");
    std::string exec_type    = parse_string(msg, "exec_type");

    // Use order_status as the canonical state; exec_type breaks ties.
    update.new_state = order_status.empty()
        ? map_exec_type(exec_type)
        : map_order_status(order_status);

    std::string reason = parse_string(msg, "reason");
    if (!reason.empty())
        std::strncpy(update.reject_reason, reason.c_str(), 63);

    LOG_INFO("Kraken execution update",
             "client_id",    update.client_order_id,
             "order_status", order_status.c_str(),
             "fill_qty",     update.fill_qty,
             "fill_px",      update.fill_price);

    emit_fill(update);
}

// ── Signing ───────────────────────────────────────────────────────────────────

std::string KrakenConnector::kraken_sign(const std::string& path,
                                          const std::string& post_data,
                                          const std::string& nonce) const {
    // Step 1: SHA256(nonce + post_data)
    std::string message = nonce + post_data;
    unsigned char sha256_out[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(message.data()),
           message.size(), sha256_out);

    // Step 2: path + sha256_result (binary concatenation)
    std::string hmac_input = path;
    hmac_input.append(reinterpret_cast<const char*>(sha256_out), SHA256_DIGEST_LENGTH);

    // Step 3: HMAC-SHA512 with base64-decoded secret
    std::vector<unsigned char> decoded_secret = base64_decode(api_secret_);

    unsigned char hmac_out[EVP_MAX_MD_SIZE];
    unsigned int  hmac_len = 0;
    HMAC(EVP_sha512(),
         decoded_secret.data(), static_cast<int>(decoded_secret.size()),
         reinterpret_cast<const unsigned char*>(hmac_input.data()), hmac_input.size(),
         hmac_out, &hmac_len);

    return base64_encode(hmac_out, hmac_len);
}

std::string KrakenConnector::base64_encode(const unsigned char* data, size_t len) const {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_push(b64, mem);
    BIO_write(b64, data, static_cast<int>(len));
    BIO_flush(b64);

    BUF_MEM* buf;
    BIO_get_mem_ptr(b64, &buf);
    std::string result(buf->data, buf->length);
    BIO_free_all(b64);
    return result;
}

std::vector<unsigned char> KrakenConnector::base64_decode(const std::string& encoded) const {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new_mem_buf(encoded.data(), static_cast<int>(encoded.size()));
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_push(b64, mem);

    std::vector<unsigned char> decoded(encoded.size());
    int len = BIO_read(b64, decoded.data(), static_cast<int>(decoded.size()));
    BIO_free_all(b64);

    if (len > 0) decoded.resize(static_cast<size_t>(len));
    else         decoded.clear();
    return decoded;
}

std::string KrakenConnector::generate_nonce() {
    uint64_t n = next_nonce_.fetch_add(1, std::memory_order_relaxed);
    return std::to_string(n);
}

int64_t KrakenConnector::now_ns() const { return http::now_ns(); }

// ── HTTP helpers ──────────────────────────────────────────────────────────────

std::string KrakenConnector::http_post_private(const std::string& path,
                                                const std::string& post_data) const {
    // Extract nonce from post_data for signing
    std::string nonce;
    std::regex nonce_re(R"(nonce=([0-9]+))");
    std::smatch m;
    if (std::regex_search(post_data, m, nonce_re)) nonce = m[1].str();

    std::string sig = kraken_sign(path, post_data, nonce);
    std::string url = api_url_ + path;

    return http::post(url, post_data, {
        "API-Key: " + api_key_,
        "API-Sign: " + sig,
        "Content-Type: application/x-www-form-urlencoded"
    });
}

std::string KrakenConnector::http_get(const std::string& url) const {
    return http::get(url);
}

// ── Symbol tracking ───────────────────────────────────────────────────────────

void KrakenConnector::track_order(uint64_t client_id, const char* txid) noexcept {
    // Update existing entry if present
    uint32_t count = tracked_count_.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i) {
        if (tracked_[i].client_id == client_id) {
            std::strncpy(tracked_[i].txid, txid, 63);
            tracked_[i].txid[63] = '\0';
            return;
        }
    }

    uint32_t idx = tracked_count_.fetch_add(1, std::memory_order_relaxed);
    if (idx >= MAX_TRACKED) {
        tracked_count_.fetch_sub(1, std::memory_order_relaxed);
        LOG_WARN("Kraken tracked order table full", "max", MAX_TRACKED);
        return;
    }
    tracked_[idx].client_id = client_id;
    std::strncpy(tracked_[idx].txid, txid, 63);
    tracked_[idx].txid[63] = '\0';
}

const char* KrakenConnector::find_txid(uint64_t client_id) const noexcept {
    uint32_t count = tracked_count_.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i)
        if (tracked_[i].client_id == client_id) return tracked_[i].txid;
    return nullptr;
}

// ── JSON helpers ──────────────────────────────────────────────────────────────

std::string KrakenConnector::parse_string(const std::string& json, const std::string& key) const {
    return http::parse_string(json, key);
}

double KrakenConnector::parse_double(const std::string& json, const std::string& key) const {
    return http::parse_double(json, key);
}

int64_t KrakenConnector::parse_int64(const std::string& json, const std::string& key) const {
    return http::parse_int64(json, key);
}

OrderState KrakenConnector::map_order_status(const std::string& status) noexcept {
    if (status == "pending_new")      return OrderState::PENDING_NEW;
    if (status == "new")              return OrderState::ACTIVE;
    if (status == "partially_filled") return OrderState::ACTIVE;
    if (status == "filled")           return OrderState::FILLED;
    if (status == "canceled")         return OrderState::CANCELED;
    if (status == "expired")          return OrderState::CANCELED;
    return OrderState::ACTIVE;
}

OrderState KrakenConnector::map_exec_type(const std::string& exec_type) noexcept {
    if (exec_type == "pending_new") return OrderState::PENDING_NEW;
    if (exec_type == "new")         return OrderState::ACTIVE;
    if (exec_type == "trade")       return OrderState::ACTIVE;
    if (exec_type == "canceled")    return OrderState::CANCELED;
    if (exec_type == "expired")     return OrderState::CANCELED;
    return OrderState::ACTIVE;
}

}  // namespace trading
