#pragma once

#include "../common/logging.hpp"
#include "../common/rest_client.hpp"
#include "exchange_connector.hpp"
#include "venue_order_map.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#if defined(TRT_DISABLE_OPENSSL)
#define TRT_HAS_OPENSSL 0
#elif defined(__has_include)
#if __has_include(<openssl/hmac.h>) && __has_include(<openssl/evp.h>)
#include <openssl/evp.h>
#include <openssl/hmac.h>
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
          api_url_(std::move(api_url)), retry_policy_(retry_policy) {}

    Exchange exchange_id() const override { return exchange_; }
    bool is_connected() const override { return connected_.load(std::memory_order_acquire); }

    ConnectorResult connect() override {
        connected_.store(true, std::memory_order_release);
        return ConnectorResult::OK;
    }

    void disconnect() override { connected_.store(false, std::memory_order_release); }

    ConnectorResult submit_order(const Order& order) override {
        const std::string idempotency_key = make_idempotency_key(order.client_order_id);
        std::string venue_order_id;
        const ConnectorResult result =
            with_retries([&]() { return submit_to_venue(order, idempotency_key, venue_order_id); });

        if (result == ConnectorResult::OK) {
            if (!order_map_.upsert(order.client_order_id, venue_order_id.c_str(), exchange_,
                                   order.symbol)) {
                LOG_ERROR("Venue order map full", "exchange", exchange_to_string(exchange_));
                return ConnectorResult::ERROR_UNKNOWN;
            }
        }
        return result;
    }

    ConnectorResult cancel_order(uint64_t client_order_id) override {
        const VenueOrderEntry* mapped = order_map_.get(client_order_id);
        if (!mapped)
            return ConnectorResult::ERROR_INVALID_ORDER;

        const ConnectorResult result = with_retries([&]() { return cancel_at_venue(*mapped); });

        if (result == ConnectorResult::OK) {
            order_map_.erase(client_order_id);
        }
        return result;
    }

    ConnectorResult replace_order(uint64_t client_order_id, const Order& replacement) override {
        const VenueOrderEntry* mapped = order_map_.get(client_order_id);
        if (!mapped)
            return ConnectorResult::ERROR_INVALID_ORDER;

        std::string new_venue_order_id;
        const ConnectorResult result = with_retries(
            [&]() { return replace_at_venue(*mapped, replacement, new_venue_order_id); });
        if (result != ConnectorResult::OK)
            return result;

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
        order_map_.clear();
        return ConnectorResult::OK;
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
        return code == ConnectorResult::ERROR_RATE_LIMIT || code == ConnectorResult::ERROR_UNKNOWN;
    }

    std::string make_idempotency_key(uint64_t client_order_id) const {
        return std::string("TRT-") + std::to_string(client_order_id) + "-" +
               exchange_to_string(exchange_);
    }

    std::vector<std::string> auth_headers(const std::string& payload,
                                          const std::string& idempotency_key = "") const {
        const int64_t ts_ms = http::now_ms();
        const std::string ts = std::to_string(ts_ms);
        const std::string signature = sign_payload(ts + payload);
        std::vector<std::string> headers = {"X-API-KEY: " + api_key_, "X-TS: " + ts,
                                            "X-SIGN: " + signature,
                                            "Content-Type: application/json"};
        if (!idempotency_key.empty())
            headers.push_back("X-IDEMPOTENCY-KEY: " + idempotency_key);
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

    std::string sign_payload(const std::string& payload) const {
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
        const size_t h = std::hash<std::string>{}(api_secret_ + payload);
        return std::to_string(static_cast<unsigned long long>(h));
#endif
    }

    Exchange exchange_;
    std::string api_key_;
    std::string api_secret_;
    std::string api_url_;
    RetryPolicy retry_policy_;
    std::atomic<bool> connected_{false};
    VenueOrderMap order_map_{};
};

} // namespace trading
