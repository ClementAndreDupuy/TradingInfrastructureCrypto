#pragma once

#include "../../common/logging.hpp"
#include "../../common/rest_client.hpp"
#include "exchange_connector.hpp"
#include "idempotency_journal.hpp"
#include "reconciliation_types.hpp"
#include "venue_order_map.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <cstring>
#include <string>
#include <array>
#include <thread>
#include <utility>
#include <vector>

#if defined(TRT_DISABLE_OPENSSL)
#define TRT_HAS_OPENSSL 0
#elif defined(__has_include)
#if __has_include(<openssl/hmac.h>) && __has_include(<openssl/evp.h>)
#include <openssl/bn.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
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
                          std::string api_url, RetryPolicy retry_policy = {},
                          std::string api_passphrase = {})
            : exchange_(exchange), api_key_(std::move(api_key)), api_secret_(std::move(api_secret)),
              api_passphrase_(std::move(api_passphrase)), api_url_(std::move(api_url)),
              retry_policy_(retry_policy),
              journal_(std::string("/tmp/trt_idempotency_") + exchange_to_string(exchange_) + ".log") {
        }

        Exchange exchange_id() const override { return exchange_; }
        bool is_connected() const override { return connected_.load(std::memory_order_acquire); }

        ConnectorResult connect() override {
#if !TRT_HAS_OPENSSL
            LOG_ERROR("OpenSSL backend unavailable for live connector", "exchange",
                      exchange_to_string(exchange_));
            connected_.store(false, std::memory_order_release);
            return ConnectorResult::AUTH_FAILED;
#endif
            if (exchange_ == Exchange::OKX && api_passphrase_.empty()) {
                LOG_ERROR("Missing venue credential", "exchange", exchange_to_string(exchange_),
                          "field", "passphrase");
                connected_.store(false, std::memory_order_release);
                return ConnectorResult::AUTH_FAILED;
            }
            connected_.store(true, std::memory_order_release);
            return ConnectorResult::OK;
        }

        void disconnect() override { connected_.store(false, std::memory_order_release); }

        ConnectorResult submit_order(const Order &order) override {
            const JournalDecision recovery =
                    journal_.begin(JournalOperation::SUBMIT, order.client_order_id);
            if (recovery.already_acked()) {
                if (recovery.venue_order_id[0] != '\0' && recovery.venue_order_id[0] != '-') {
                    if (!venue_order_map_.upsert(order.client_order_id, recovery.venue_order_id,
                                                 exchange_, order.symbol)) {
                        LOG_ERROR("Venue order map full", "exchange", exchange_to_string(exchange_));
                        return ConnectorResult::ERROR_UNKNOWN;
                    }
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
                if (!venue_order_map_.upsert(order.client_order_id, venue_order_id.c_str(), exchange_,
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
            const JournalDecision replace_state =
                    journal_.lookup(JournalOperation::REPLACE, client_order_id);
            if (replace_state.state == JournalState::IN_FLIGHT ||
                replace_state.state == JournalState::ACKED)
                return ConnectorResult::ERROR_INVALID_ORDER;

            const JournalDecision decision = journal_.begin(JournalOperation::CANCEL, client_order_id);
            if (decision.already_acked())
                return ConnectorResult::OK;
            if (!decision.should_send_to_venue())
                return ConnectorResult::ERROR_UNKNOWN;

            const VenueOrderEntry *mapped = venue_order_map_.get(client_order_id);
            if (!mapped) {
                journal_.fail(JournalOperation::CANCEL, client_order_id);
                return ConnectorResult::ERROR_INVALID_ORDER;
            }

            const ConnectorResult result = with_retries([&]() { return cancel_at_venue(*mapped); });

            if (result == ConnectorResult::OK) {
                journal_.ack(JournalOperation::CANCEL, client_order_id, mapped->venue_order_id);
                venue_order_map_.erase(client_order_id);
            } else {
                journal_.fail(JournalOperation::CANCEL, client_order_id);
            }
            return result;
        }

        ConnectorResult replace_order(uint64_t client_order_id, const Order &replacement) override {
            const JournalDecision cancel_state =
                    journal_.lookup(JournalOperation::CANCEL, client_order_id);
            if (cancel_state.state == JournalState::IN_FLIGHT ||
                cancel_state.state == JournalState::ACKED)
                return ConnectorResult::ERROR_INVALID_ORDER;

            const VenueOrderEntry *mapped = venue_order_map_.get(client_order_id);
            if (!mapped)
                return ConnectorResult::ERROR_INVALID_ORDER;

            const JournalDecision decision =
                    journal_.begin(JournalOperation::REPLACE, client_order_id, replacement.client_order_id);
            if (decision.already_acked()) {
                if (decision.venue_order_id[0] != '\0' && decision.venue_order_id[0] != '-') {
                    venue_order_map_.erase(client_order_id);
                    if (!venue_order_map_.upsert(replacement.client_order_id, decision.venue_order_id,
                                                 exchange_, replacement.symbol)) {
                        LOG_ERROR("Venue order map full", "exchange", exchange_to_string(exchange_));
                        return ConnectorResult::ERROR_UNKNOWN;
                    }
                }
                return ConnectorResult::OK;
            }
            if (!decision.should_send_to_venue())
                return ConnectorResult::ERROR_UNKNOWN;

            std::string new_venue_order_id;
            const ConnectorResult result = with_retries(
                [&]() { return replace_at_venue(*mapped, replacement, new_venue_order_id); });
            if (result != ConnectorResult::OK) {
                journal_.fail(JournalOperation::REPLACE, client_order_id, replacement.client_order_id);
                return result;
            }

            journal_.ack(JournalOperation::REPLACE, client_order_id, new_venue_order_id.c_str(),
                         replacement.client_order_id);
            venue_order_map_.erase(client_order_id);
            if (!venue_order_map_.upsert(replacement.client_order_id, new_venue_order_id.c_str(),
                                         exchange_, replacement.symbol)) {
                LOG_ERROR("Venue order map full", "exchange", exchange_to_string(exchange_));
                return ConnectorResult::ERROR_UNKNOWN;
            }
            return ConnectorResult::OK;
        }

        ConnectorResult query_order(uint64_t client_order_id, FillUpdate &status) override {
            const VenueOrderEntry *mapped = venue_order_map_.get(client_order_id);
            if (!mapped)
                return ConnectorResult::ERROR_INVALID_ORDER;

            return with_retries([&]() { return query_at_venue(*mapped, status); });
        }

        ConnectorResult cancel_all(const char *symbol) override { return cancel_all_at_venue(symbol); }

        ConnectorResult reconcile() override {
            journal_.recover();
            venue_order_map_.clear();
            return ConnectorResult::OK;
        }

        uint32_t in_flight_recovery_count() const noexcept { return journal_.in_flight_count(); }

        uint64_t duplicate_ack_recovery_count() const noexcept {
            return journal_.duplicate_ack_count();
        }

        virtual ConnectorResult fetch_reconciliation_snapshot(ReconciliationSnapshot &snapshot) {
            snapshot.clear();
            return ConnectorResult::ERROR_UNKNOWN;
        }

        const VenueOrderMap &venue_order_map() const noexcept { return venue_order_map_; }

    protected:
        virtual ConnectorResult submit_to_venue(const Order &order, const std::string &idempotency_key,
                                                std::string &venue_order_id) = 0;

        virtual ConnectorResult cancel_at_venue(const VenueOrderEntry &entry) = 0;

        virtual ConnectorResult replace_at_venue(const VenueOrderEntry &entry, const Order &replacement,
                                                 std::string &new_venue_order_id) = 0;

        virtual ConnectorResult query_at_venue(const VenueOrderEntry &entry, FillUpdate &status) = 0;

        virtual ConnectorResult cancel_all_at_venue(const char *symbol) = 0;

        const std::string &api_url() const noexcept { return api_url_; }

        std::vector<std::string> binance_api_headers() const {
            return {"X-MBX-APIKEY: " + api_key_};
        }

        std::string hmac_sha256_hex_for_payload(const std::string &payload) const {
            return hmac_sha256_hex(payload);
        }

        virtual bool is_retryable(ConnectorResult code) const noexcept {
            return code == ConnectorResult::ERROR_RATE_LIMIT ||
                   code == ConnectorResult::ERROR_REST_FAILURE ||
                   code == ConnectorResult::ERROR_UNKNOWN;
        }

        static ConnectorResult classify_http_error(int status) noexcept {
            if (status == 401 || status == 403)
                return ConnectorResult::AUTH_FAILED;
            if (status == 429)
                return ConnectorResult::ERROR_RATE_LIMIT;
            if (status >= 500)
                return ConnectorResult::ERROR_REST_FAILURE;
            if (status >= 400)
                return ConnectorResult::ERROR_INVALID_ORDER;
            return ConnectorResult::ERROR_UNKNOWN;
        }

        std::string make_idempotency_key(uint64_t client_order_id) const {
            return std::string("TRT-") + std::to_string(client_order_id) + "-" +
                   exchange_to_string(exchange_);
        }

        std::vector<std::string> auth_headers(const char *method, const std::string &request_path,
                                              const std::string &payload,
                                              const std::string &idempotency_key = "") const {
            return auth_headers_with_timestamp(method, request_path, payload, http::now_ms(),
                                               idempotency_key);
        }

        std::vector<std::string> auth_headers_with_timestamp(const char *method,
                                                             const std::string &request_path,
                                                             const std::string &payload,
                                                             int64_t ts_ms,
                                                             const std::string &idempotency_key = "") const {
            const std::string ts_ms_s = std::to_string(ts_ms);
            const std::string ts_s = std::to_string(static_cast<double>(ts_ms) / 1000.0);

            std::vector<std::string> headers;
            if (!idempotency_key.empty()) {
                headers.push_back("X-IDEMPOTENCY-KEY: " + idempotency_key);
            }

            switch (exchange_) {
                case Exchange::BINANCE: {
                    headers.push_back("Content-Type: application/json");
                    const std::string prehash = ts_ms_s + method + request_path + payload;
                    headers.push_back("X-MBX-APIKEY: " + api_key_);
                    headers.push_back("X-MBX-TIMESTAMP: " + ts_ms_s);
                    headers.push_back("X-MBX-SIGNATURE: " + hmac_sha256_hex(prehash));
                    break;
                }
                case Exchange::KRAKEN:
                    headers.push_back("Content-Type: application/x-www-form-urlencoded; charset=utf-8");
                    break;
                case Exchange::OKX: {
                    headers.push_back("Content-Type: application/json");
                    const std::string prehash = ts_s + method + request_path + payload;
                    headers.push_back("OK-ACCESS-KEY: " + api_key_);
                    headers.push_back("OK-ACCESS-TIMESTAMP: " + ts_s);
                    headers.push_back("OK-ACCESS-PASSPHRASE: " + api_passphrase_);
                    headers.push_back("OK-ACCESS-SIGN: " + hmac_sha256_base64(prehash));
                    break;
                }
                case Exchange::COINBASE: {
                    headers.push_back("Content-Type: application/json");
                    headers.push_back("Accept: application/json");
                    const std::string jwt = coinbase_bearer_token(method, request_path);
                    if (!jwt.empty()) {
                        headers.push_back("Authorization: Bearer " + jwt);
                    }
                    break;
                }
                default:
                    break;
            }
            return headers;
        }


        std::string coinbase_bearer_token(const char *method, const std::string &request_path) const {
#if TRT_HAS_OPENSSL
            if (api_key_.empty() || api_secret_.empty())
                return std::string();

            const std::string host = extract_host(api_url_);
            if (host.empty())
                return std::string();

            const int64_t now_s = http::now_ns() / 1000000000LL;
            const std::string header = R"({"alg":"ES256","kid":"")" + json_escape(api_key_) +
                                       R"(","typ":"JWT"})";
            const std::string payload =
                    R"({"sub":")" + json_escape(api_key_) + R"(","iss":"cdp","nbf":)" +
                    std::to_string(now_s) + R"(,"exp":)" + std::to_string(now_s + 120) +
                    R"(,"uri":")" + json_escape(std::string(method) + " " + host + request_path) +
                    R"("})";
            const std::string signing_input =
                    base64url_encode(header) + "." + base64url_encode(payload);

            BIO *bio = BIO_new_mem_buf(api_secret_.data(), static_cast<int>(api_secret_.size()));
            if (bio == nullptr)
                return std::string();
            EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
            BIO_free(bio);
            if (key == nullptr)
                return std::string();

            EVP_MD_CTX *ctx = EVP_MD_CTX_new();
            if (ctx == nullptr) {
                EVP_PKEY_free(key);
                return std::string();
            }

            std::string jwt;
            size_t der_len = 0;
            if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1 &&
                EVP_DigestSignUpdate(ctx, signing_input.data(), signing_input.size()) == 1 &&
                EVP_DigestSignFinal(ctx, nullptr, &der_len) == 1) {
                std::vector<unsigned char> der(der_len);
                if (EVP_DigestSignFinal(ctx, der.data(), &der_len) == 1) {
                    der.resize(der_len);
                    const std::string signature = ecdsa_der_to_jose(der.data(), der.size());
                    if (!signature.empty())
                        jwt = signing_input + "." + signature;
                }
            }

            EVP_MD_CTX_free(ctx);
            EVP_PKEY_free(key);
            return jwt;
#else
            (void) method;
            (void) request_path;
            return std::string();
#endif
        }

        std::vector<std::string> kraken_private_headers(const std::string &request_path,
                                                        const std::string &encoded_payload) const {
            return {
                "Content-Type: application/x-www-form-urlencoded; charset=utf-8",
                "API-Key: " + api_key_,
                "API-Sign: " + kraken_api_sign(request_path, encoded_payload),
            };
        }

    private:
        template<typename Fn>
        ConnectorResult with_retries(Fn &&fn) {
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

        std::string hmac_sha256_hex(const std::string &payload) const {
#if TRT_HAS_OPENSSL
            return encode_hex(compute_hmac(EVP_sha256(), payload));
#else
            (void) payload;
            return std::string();
#endif
        }

        std::string hmac_sha256_base64(const std::string &payload) const {
#if TRT_HAS_OPENSSL
            return encode_base64(compute_hmac(EVP_sha256(), payload));
#else
            (void) payload;
            return std::string();
#endif
        }

        std::string hmac_sha512_base64(const std::string &payload) const {
#if TRT_HAS_OPENSSL
            return encode_base64(compute_hmac(EVP_sha512(), payload));
#else
            (void) payload;
            return std::string();
#endif
        }

        std::string kraken_api_sign(const std::string &request_path,
                                    const std::string &encoded_payload) const {
#if TRT_HAS_OPENSSL
            const std::string nonce = extract_kraken_nonce(encoded_payload);
            if (nonce.empty())
                return std::string();

            unsigned char digest[SHA256_DIGEST_LENGTH] = {};
            const std::string nonce_and_payload = nonce + encoded_payload;
            SHA256(reinterpret_cast<const unsigned char *>(nonce_and_payload.data()),
                   nonce_and_payload.size(), digest);

            std::string message = request_path;
            message.append(reinterpret_cast<const char *>(digest), SHA256_DIGEST_LENGTH);
            return encode_base64(compute_hmac_base64_secret(EVP_sha512(), message));
#else
            (void) request_path;
            (void) encoded_payload;
            return std::string();
#endif
        }

#if TRT_HAS_OPENSSL
        struct HmacDigest {
            unsigned char data[EVP_MAX_MD_SIZE] = {};
            unsigned int len = 0;
        };

        HmacDigest compute_hmac(const EVP_MD *algo, const std::string &payload) const {
            HmacDigest result;
            HMAC(algo, api_secret_.data(), static_cast<int>(api_secret_.size()),
                 reinterpret_cast<const unsigned char *>(payload.data()), payload.size(), result.data,
                 &result.len);
            return result;
        }

        HmacDigest compute_hmac_base64_secret(const EVP_MD *algo, const std::string &payload) const {
            HmacDigest result;
            const std::string decoded_secret = decode_base64(api_secret_);
            HMAC(algo, decoded_secret.data(), static_cast<int>(decoded_secret.size()),
                 reinterpret_cast<const unsigned char *>(payload.data()), payload.size(), result.data,
                 &result.len);
            return result;
        }

        static std::string encode_hex(const HmacDigest &d) { return encode_hex(d.data, d.len); }


        static std::string extract_host(const std::string &url) {
            const size_t scheme = url.find("://");
            const size_t host_start = scheme == std::string::npos ? 0 : scheme + 3;
            size_t host_end = url.find('/', host_start);
            if (host_end == std::string::npos)
                host_end = url.size();
            return url.substr(host_start, host_end - host_start);
        }

        static std::string json_escape(const std::string &input) {
            std::string out;
            out.reserve(input.size());
            for (char ch: input) {
                if (ch == '\\' || ch == '"')
                    out.push_back('\\');
                out.push_back(ch);
            }
            return out;
        }

        static std::string base64url_encode(const std::string &data) {
            return base64url_encode(reinterpret_cast<const unsigned char *>(data.data()), data.size());
        }

        static std::string base64url_encode(const unsigned char *data, size_t len) {
            static constexpr char k_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
            std::string out;
            out.reserve(((len + 2U) / 3U) * 4U);
            for (size_t i = 0; i < len; i += 3U) {
                const unsigned int octet_a = data[i];
                const unsigned int octet_b = (i + 1U < len) ? data[i + 1U] : 0U;
                const unsigned int octet_c = (i + 2U < len) ? data[i + 2U] : 0U;
                const unsigned int triple = (octet_a << 16U) | (octet_b << 8U) | octet_c;
                out.push_back(k_chars[(triple >> 18U) & 0x3FU]);
                out.push_back(k_chars[(triple >> 12U) & 0x3FU]);
                if (i + 1U < len)
                    out.push_back(k_chars[(triple >> 6U) & 0x3FU]);
                if (i + 2U < len)
                    out.push_back(k_chars[triple & 0x3FU]);
            }
            return out;
        }

#if TRT_HAS_OPENSSL
        static std::string ecdsa_der_to_jose(const unsigned char *der, size_t der_len) {
            const unsigned char *cursor = der;
            ECDSA_SIG *sig = d2i_ECDSA_SIG(nullptr, &cursor, static_cast<long>(der_len));
            if (sig == nullptr)
                return std::string();
            const BIGNUM *r = nullptr;
            const BIGNUM *s = nullptr;
            ECDSA_SIG_get0(sig, &r, &s);
            if (r == nullptr || s == nullptr) {
                ECDSA_SIG_free(sig);
                return std::string();
            }
            std::array<unsigned char, 64> raw = {};
            if (BN_bn2binpad(r, raw.data(), 32) != 32 || BN_bn2binpad(s, raw.data() + 32, 32) != 32) {
                ECDSA_SIG_free(sig);
                return std::string();
            }
            ECDSA_SIG_free(sig);
            return base64url_encode(raw.data(), raw.size());
        }
#endif

        static std::string extract_kraken_nonce(const std::string &encoded_payload) {
            const std::string key = "nonce=";
            const std::size_t pos = encoded_payload.find(key);
            if (pos == std::string::npos)
                return std::string();
            const std::size_t start = pos + key.size();
            const std::size_t end = encoded_payload.find('&', start);
            return encoded_payload.substr(start, end == std::string::npos
                                                     ? std::string::npos
                                                     : end - start);
        }

        static std::string decode_base64(const std::string &encoded) {
            BIO *b64 = BIO_new(BIO_f_base64());
            BIO *mem = BIO_new_mem_buf(encoded.data(), static_cast<int>(encoded.size()));
            BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
            mem = BIO_push(b64, mem);

            std::string decoded(encoded.size(), '\0');
            const int len = BIO_read(mem, decoded.data(), static_cast<int>(decoded.size()));
            BIO_free_all(mem);
            if (len <= 0)
                return std::string();
            decoded.resize(static_cast<std::size_t>(len));
            return decoded;
        }

        static std::string encode_hex(const unsigned char *data, unsigned int len) {
            static const char hex_chars[] = "0123456789abcdef";
            std::string out;
            out.resize(len * 2);
            for (unsigned int i = 0; i < len; ++i) {
                out[2 * i] = hex_chars[(data[i] >> 4) & 0xF];
                out[2 * i + 1] = hex_chars[data[i] & 0xF];
            }
            return out;
        }

        static std::string encode_base64(const HmacDigest &d) {
            std::string out;
            out.resize((d.len + 2) / 3 * 4);
            const int out_len = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(&out[0]), d.data,
                                                static_cast<int>(d.len));
            out.resize(static_cast<size_t>(out_len));
            return out;
        }
#endif

        Exchange exchange_;
        std::string api_key_;
        std::string api_secret_;
        std::string api_passphrase_;
        std::string api_url_;
        RetryPolicy retry_policy_;
        std::atomic<bool> connected_{false};
        VenueOrderMap venue_order_map_{};
        IdempotencyJournal journal_;
    };
}
