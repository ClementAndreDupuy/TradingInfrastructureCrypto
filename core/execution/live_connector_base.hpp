#pragma once

#include "../common/logging.hpp"
#include "../common/rest_client.hpp"
#include "exchange_connector.hpp"
#include "idempotency_journal.hpp"
#include "reconciliation_types.hpp"
#include "venue_order_map.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(TRT_DISABLE_OPENSSL)
#define TRT_HAS_OPENSSL 0
#elif defined(__has_include)
#if __has_include(<openssl/hmac.h>) && __has_include(<openssl/evp.h>)
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#define TRT_HAS_OPENSSL 1
#else
#define TRT_HAS_OPENSSL 0
#endif
#else
#define TRT_HAS_OPENSSL 0
#endif

namespace trading {

struct RetryPolicy {
    int max_attempts = 3;
    int backoff_ms = 20;
};

class LiveConnectorBase : public ExchangeConnector {
  public:
    LiveConnectorBase(Exchange exchange, std::string api_key, std::string api_secret,
                      std::string api_url, RetryPolicy retry_policy = {})
        : exchange_(exchange), api_key_(std::move(api_key)), api_secret_(std::move(api_secret)),
          api_url_(std::move(api_url)), retry_policy_(retry_policy),
          journal_(std::string("/tmp/trt_idempotency_") + exchange_to_string(exchange_) + ".log") {}

    Exchange exchange_id() const override { return exchange_; }
    bool is_connected() const override { return connected_.load(std::memory_order_acquire); }

    ConnectorResult connect() override {
#if !TRT_HAS_OPENSSL
        LOG_ERROR("OpenSSL backend unavailable for live connector", "exchange",
                  exchange_to_string(exchange_));
        connected_.store(false, std::memory_order_release);
        return ConnectorResult::AUTH_FAILED;
#endif
        connected_.store(true, std::memory_order_release);
        return ConnectorResult::OK;
    }

    void disconnect() override { connected_.store(false, std::memory_order_release); }

    ConnectorResult submit_order(const Order& order) override {
        const JournalDecision recovery = journal_.begin(JournalOperation::SUBMIT, order.client_order_id);
        if (recovery.already_acked()) {
            if (recovery.venue_order_id[0] != '\0' && recovery.venue_order_id[0] != '-') {
                order_map_.upsert(order.client_order_id, recovery.venue_order_id, exchange_, order.symbol);
            }
            return ConnectorResult::OK;
        }
        if (!recovery.should_send_to_venue())
            return ConnectorResult::ERROR_UNKNOWN;

        const std::string idempotency_key = make_idempotency_key(order.client_order_id);
        std::string venue_order_id;
        const ConnectorResult result =
            with_retries([&]() { return submit_to_venue(order, idempotency_key, venue_order_id); });

        if (result == ConnectorResult::OK) {
            journal_.ack(JournalOperation::SUBMIT, order.client_order_id, venue_order_id.c_str());
            if (!order_map_.upsert(order.client_order_id, venue_order_id.c_str(), exchange_,
                                   order.symbol)) {
                LOG_ERROR("Venue order map full", "exchange", exchange_to_string(exchange_));
                return ConnectorResult::ERROR_UNKNOWN;
            }
        } else {
            journal_.fail(JournalOperation::SUBMIT, order.client_order_id);
        }
        return result;
    }

    ConnectorResult cancel_order(uint64_t client_order_id) override {
        const JournalDecision replace_state = journal_.lookup(JournalOperation::REPLACE, client_order_id);
        if (replace_state.state == JournalState::IN_FLIGHT || replace_state.state == JournalState::ACKED)
            return ConnectorResult::ERROR_INVALID_ORDER;

        const JournalDecision decision = journal_.begin(JournalOperation::CANCEL, client_order_id);
        if (decision.already_acked())
            return ConnectorResult::OK;
        if (!decision.should_send_to_venue())
            return ConnectorResult::ERROR_UNKNOWN;

        const VenueOrderEntry* mapped = order_map_.get(client_order_id);
        if (!mapped)
            return ConnectorResult::ERROR_INVALID_ORDER;

        const ConnectorResult result = with_retries([&]() { return cancel_at_venue(*mapped); });

        if (result == ConnectorResult::OK) {
            journal_.ack(JournalOperation::CANCEL, client_order_id, mapped->venue_order_id);
            order_map_.erase(client_order_id);
        } else {
            journal_.fail(JournalOperation::CANCEL, client_order_id);
        }
        return result;
    }

    ConnectorResult replace_order(uint64_t client_order_id, const Order& replacement) override {
        const JournalDecision cancel_state = journal_.lookup(JournalOperation::CANCEL, client_order_id);
        if (cancel_state.state == JournalState::IN_FLIGHT || cancel_state.state == JournalState::ACKED)
            return ConnectorResult::ERROR_INVALID_ORDER;

        const JournalDecision decision =
            journal_.begin(JournalOperation::REPLACE, client_order_id, replacement.client_order_id);
        if (decision.already_acked()) {
            if (decision.venue_order_id[0] != '\0' && decision.venue_order_id[0] != '-') {
                order_map_.erase(client_order_id);
                order_map_.upsert(replacement.client_order_id, decision.venue_order_id, exchange_,
                                  replacement.symbol);
            }
            return ConnectorResult::OK;
        }
        if (!decision.should_send_to_venue())
            return ConnectorResult::ERROR_UNKNOWN;

        const VenueOrderEntry* mapped = order_map_.get(client_order_id);
        if (!mapped)
            return ConnectorResult::ERROR_INVALID_ORDER;

        std::string new_venue_order_id;
        const ConnectorResult result = with_retries(
            [&]() { return replace_at_venue(*mapped, replacement, new_venue_order_id); });
        if (result != ConnectorResult::OK) {
            journal_.fail(JournalOperation::REPLACE, client_order_id, replacement.client_order_id);
            return result;
        }

        journal_.ack(JournalOperation::REPLACE, client_order_id, new_venue_order_id.c_str(),
                     replacement.client_order_id);
        order_map_.erase(client_order_id);
        if (!order_map_.upsert(replacement.client_order_id, new_venue_order_id.c_str(), exchange_,
                               replacement.symbol)) {
            LOG_ERROR("Venue order map full", "exchange", exchange_to_string(exchange_));
            return ConnectorResult::ERROR_UNKNOWN;
        }
        return ConnectorResult::OK;
    }

    ConnectorResult query_order(uint64_t client_order_id, FillUpdate& status) override {
        const VenueOrderEntry* mapped = order_map_.get(client_order_id);
        if (!mapped)
            return ConnectorResult::ERROR_INVALID_ORDER;

        return with_retries([&]() { return query_at_venue(*mapped, status); });
    }

    ConnectorResult cancel_all(const char* symbol) override { return cancel_all_at_venue(symbol); }

    ConnectorResult reconcile() override {
        journal_.recover();
        order_map_.clear();
        return ConnectorResult::OK;
    }

    uint32_t in_flight_recovery_count() const noexcept { return journal_.in_flight_count(); }

    virtual ConnectorResult fetch_reconciliation_snapshot(ReconciliationSnapshot& snapshot) {
        snapshot.clear();
        return ConnectorResult::ERROR_UNKNOWN;
    }

    const VenueOrderMap& order_map() const noexcept { return order_map_; }

  protected:
    virtual ConnectorResult submit_to_venue(const Order& order, const std::string& idempotency_key,
                                            std::string& venue_order_id) = 0;
    virtual ConnectorResult cancel_at_venue(const VenueOrderEntry& entry) = 0;
    virtual ConnectorResult replace_at_venue(const VenueOrderEntry& entry, const Order& replacement,
                                             std::string& new_venue_order_id) = 0;
    virtual ConnectorResult query_at_venue(const VenueOrderEntry& entry, FillUpdate& status) = 0;
    virtual ConnectorResult cancel_all_at_venue(const char* symbol) = 0;
    virtual bool is_retryable(ConnectorResult code) const noexcept {
        return code == ConnectorResult::ERROR_RATE_LIMIT ||
               code == ConnectorResult::ERROR_REST_FAILURE ||
               code == ConnectorResult::ERROR_UNKNOWN;
    }

    std::string make_idempotency_key(uint64_t client_order_id) const {
        return std::string("TRT-") + std::to_string(client_order_id) + "-" +
               exchange_to_string(exchange_);
    }

    std::vector<std::string> auth_headers(const char* method, const std::string& request_path,
                                          const std::string& payload,
                                          const std::string& idempotency_key = "") const {
        const int64_t ts_ms = http::now_ms();
        const std::string ts_ms_s = std::to_string(ts_ms);
        const std::string ts_s = std::to_string(static_cast<double>(ts_ms) / 1000.0);

        std::vector<std::string> headers = {"Content-Type: application/json"};
        if (!idempotency_key.empty()) {
            headers.push_back("X-IDEMPOTENCY-KEY: " + idempotency_key);
        }

        switch (exchange_) {
        case Exchange::BINANCE: {
            const std::string prehash = ts_ms_s + method + request_path + payload;
            headers.push_back("X-MBX-APIKEY: " + api_key_);
            headers.push_back("X-MBX-TIMESTAMP: " + ts_ms_s);
            headers.push_back("X-MBX-SIGNATURE: " + hmac_sha256_hex(prehash));
            break;
        }
        case Exchange::KRAKEN: {
            const std::string prehash = request_path + sha256_hex(ts_ms_s + payload);
            headers.push_back("API-Key: " + api_key_);
            headers.push_back("API-Nonce: " + ts_ms_s);
            headers.push_back("API-Sign: " + hmac_sha512_base64(prehash));
            break;
        }
        case Exchange::OKX: {
            const std::string prehash = ts_s + method + request_path + payload;
            headers.push_back("OK-ACCESS-KEY: " + api_key_);
            headers.push_back("OK-ACCESS-TIMESTAMP: " + ts_s);
            headers.push_back("OK-ACCESS-SIGN: " + hmac_sha256_base64(prehash));
            break;
        }
        case Exchange::COINBASE: {
            const std::string prehash = ts_s + method + request_path + payload;
            headers.push_back("CB-ACCESS-KEY: " + api_key_);
            headers.push_back("CB-ACCESS-TIMESTAMP: " + ts_s);
            headers.push_back("CB-ACCESS-SIGN: " + hmac_sha256_base64(prehash));
            break;
        }
        default:
            break;
        }
        return headers;
    }

    const std::string& api_url() const noexcept { return api_url_; }

  private:
    template <typename Fn> ConnectorResult with_retries(Fn&& fn) {
        for (int attempt = 1; attempt <= retry_policy_.max_attempts; ++attempt) {
            ConnectorResult res = fn();
            if (res == ConnectorResult::OK)
                return res;
            if (!is_retryable(res) || attempt == retry_policy_.max_attempts)
                return res;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(retry_policy_.backoff_ms * attempt));
        }
        return ConnectorResult::ERROR_UNKNOWN;
    }

    std::string hmac_sha256_hex(const std::string& payload) const {
#if TRT_HAS_OPENSSL
        unsigned char digest[EVP_MAX_MD_SIZE] = {};
        unsigned int digest_len = 0;
        HMAC(EVP_sha256(), api_secret_.data(), static_cast<int>(api_secret_.size()),
             reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), digest,
             &digest_len);

        static const char* hex = "0123456789abcdef";
        std::string out;
        out.resize(digest_len * 2);
        for (unsigned int i = 0; i < digest_len; ++i) {
            out[2 * i] = hex[(digest[i] >> 4) & 0xF];
            out[2 * i + 1] = hex[digest[i] & 0xF];
        }
        return out;
#else
        (void)payload;
        return std::string();
#endif
    }

    std::string hmac_sha256_base64(const std::string& payload) const {
#if TRT_HAS_OPENSSL
        unsigned char digest[EVP_MAX_MD_SIZE] = {};
        unsigned int digest_len = 0;
        HMAC(EVP_sha256(), api_secret_.data(), static_cast<int>(api_secret_.size()),
             reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), digest,
             &digest_len);

        std::string out;
        out.resize((digest_len + 2) / 3 * 4);
        const int out_len = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]), digest,
                                            static_cast<int>(digest_len));
        out.resize(static_cast<size_t>(out_len));
        return out;
#else
        (void)payload;
        return std::string();
#endif
    }

    std::string hmac_sha512_base64(const std::string& payload) const {
#if TRT_HAS_OPENSSL
        unsigned char digest[EVP_MAX_MD_SIZE] = {};
        unsigned int digest_len = 0;
        HMAC(EVP_sha512(), api_secret_.data(), static_cast<int>(api_secret_.size()),
             reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), digest,
             &digest_len);

        std::string out;
        out.resize((digest_len + 2) / 3 * 4);
        const int out_len = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]), digest,
                                            static_cast<int>(digest_len));
        out.resize(static_cast<size_t>(out_len));
        return out;
#else
        (void)payload;
        return std::string();
#endif
    }

    std::string sha256_hex(const std::string& payload) const {
#if TRT_HAS_OPENSSL
        unsigned char digest[SHA256_DIGEST_LENGTH] = {};
        SHA256(reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), digest);

        static const char* hex = "0123456789abcdef";
        std::string out;
        out.resize(SHA256_DIGEST_LENGTH * 2);
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
            out[2 * i] = hex[(digest[i] >> 4) & 0xF];
            out[2 * i + 1] = hex[digest[i] & 0xF];
        }
        return out;
#else
        (void)payload;
        return std::string();
#endif
    }

    Exchange exchange_;
    std::string api_key_;
    std::string api_secret_;
    std::string api_url_;
    RetryPolicy retry_policy_;
    std::atomic<bool> connected_{false};
    VenueOrderMap order_map_{};
    IdempotencyJournal journal_;
};

} // namespace trading
