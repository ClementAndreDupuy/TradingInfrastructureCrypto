#include "coinbase_feed_handler.hpp"
#include "../../common/rest_client.hpp"
#include "../common/feed_handler_utils.hpp"
#include "../common/tick_size.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <ctime>
#include <exception>
#include <iomanip>
#include <libwebsockets.h>
#include <random>
#include <sstream>
#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

namespace trading {
namespace {

static auto base64url_encode(const unsigned char* data, size_t len) -> std::string {
    static constexpr char k_chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((len + 2U) / 3U) * 4U);
    for (size_t i = 0; i < len; i += 3U) {
        const unsigned int octet_a = data[i];
        const unsigned int octet_b = (i + 1U < len) ? data[i + 1U] : 0U;
        const unsigned int octet_c = (i + 2U < len) ? data[i + 2U] : 0U;
        const unsigned int triple = (octet_a << 16U) | (octet_b << 8U) | octet_c;
        out.push_back(k_chars[(triple >> 18U) & 0x3FU]);
        out.push_back(k_chars[(triple >> 12U) & 0x3FU]);
        if (i + 1U < len) {
            out.push_back(k_chars[(triple >> 6U) & 0x3FU]);
        }
        if (i + 2U < len) {
            out.push_back(k_chars[triple & 0x3FU]);
        }
    }
    return out;
}

static auto base64url_encode(const std::string& data) -> std::string {
    return base64url_encode(reinterpret_cast<const unsigned char*>(data.data()), data.size());
}

static auto random_hex(size_t bytes) -> std::string {
    static constexpr char k_hex[] = "0123456789abcdef";
    std::string out(bytes * 2U, '0');
    std::vector<unsigned char> buf(bytes);
    if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) == 1) {
        for (size_t i = 0; i < bytes; ++i) {
            out[2U * i] = k_hex[(buf[i] >> 4U) & 0xFU];
            out[2U * i + 1U] = k_hex[buf[i] & 0xFU];
        }
        return out;
    }
    std::random_device rd;
    for (size_t i = 0; i < bytes; ++i) {
        const unsigned char b = static_cast<unsigned char>(rd());
        out[2U * i] = k_hex[(b >> 4U) & 0xFU];
        out[2U * i + 1U] = k_hex[b & 0xFU];
    }
    return out;
}


static auto ecdsa_der_to_jose(const unsigned char* der, size_t der_len) -> std::string {
    const unsigned char* cursor = der;
    ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &cursor, static_cast<long>(der_len));
    if (sig == nullptr) {
        return std::string();
    }

    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
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

static auto parse_rfc3339_ns(const std::string& value) -> int64_t {
    if (value.size() < 20U) {
        return 0;
    }
    std::tm tm = {};
    std::istringstream ss(value.substr(0, 19));
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) {
        return 0;
    }
    int64_t nanos = 0;
    size_t pos = 19U;
    if (pos < value.size() && value[pos] == '.') {
        ++pos;
        size_t digits = 0;
        while (pos < value.size() && std::isdigit(static_cast<unsigned char>(value[pos])) &&
               digits < 9U) {
            nanos = nanos * 10 + static_cast<int64_t>(value[pos] - '0');
            ++pos;
            ++digits;
        }
        while (digits < 9U) {
            nanos *= 10;
            ++digits;
        }
        while (pos < value.size() && std::isdigit(static_cast<unsigned char>(value[pos]))) {
            ++pos;
        }
    }
    time_t seconds = timegm(&tm);
    if (seconds < 0) {
        return 0;
    }
    return static_cast<int64_t>(seconds) * 1'000'000'000LL + nanos;
}

}

CoinbaseFeedHandler::CoinbaseFeedHandler(const std::string& symbol, const std::string& ws_url,
                                         const std::string& api_url)
    : symbol_(symbol), ws_url_(ws_url), api_url_(api_url),
      venue_symbols_(SymbolMapper::map_all(symbol)) {
    LOG_INFO("[Coinbase] FeedHandler created", "symbol", symbol_.c_str());
}

CoinbaseFeedHandler::~CoinbaseFeedHandler() { stop(); }

auto CoinbaseFeedHandler::coinbase_api_key_from_env() -> std::string {
    std::string value = http::env_var("COINBASE_API_KEY");
    if (!value.empty()) {
        return value;
    }
    value = http::env_var("LIVE_COINBASE_API_KEY");
    if (!value.empty()) {
        return value;
    }
    return http::env_var("SHADOW_COINBASE_API_KEY");
}

auto CoinbaseFeedHandler::coinbase_api_secret_from_env() -> std::string {
    std::string value = http::env_var("COINBASE_API_SECRET");
    if (!value.empty()) {
        return value;
    }
    value = http::env_var("LIVE_COINBASE_API_SECRET");
    if (!value.empty()) {
        return value;
    }
    return http::env_var("SHADOW_COINBASE_API_SECRET");
}

auto CoinbaseFeedHandler::generate_jwt(const std::string& api_key, const std::string& api_secret)
    -> std::string {
    if (api_key.empty() || api_secret.empty()) {
        return std::string();
    }

    const int64_t now_s = http::now_ns() / 1'000'000'000LL;
    nlohmann::json header = {{"typ", "JWT"}, {"alg", "ES256"}, {"kid", api_key}, {"nonce", random_hex(16)}};
    nlohmann::json payload = {
        {"iss", "cdp"},
        {"nbf", now_s},
        {"exp", now_s + 120},
        {"sub", api_key},
    };

    const std::string signing_input = base64url_encode(header.dump()) + "." +
                                      base64url_encode(payload.dump());

    std::string pem_secret = api_secret;
    {
        size_t pos = 0;
        while ((pos = pem_secret.find("\\n", pos)) != std::string::npos) {
            pem_secret.replace(pos, 2, "\n");
            ++pos;
        }
    }

    BIO* bio = BIO_new_mem_buf(pem_secret.data(), static_cast<int>(pem_secret.size()));
    if (bio == nullptr) {
        return std::string();
    }
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (key == nullptr) {
        return std::string();
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        EVP_PKEY_free(key);
        return std::string();
    }

    size_t der_len = 0;
    std::string jwt;
    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1 &&
        EVP_DigestSignUpdate(ctx, signing_input.data(), signing_input.size()) == 1 &&
        EVP_DigestSignFinal(ctx, nullptr, &der_len) == 1) {
        std::vector<unsigned char> der(der_len);
        if (EVP_DigestSignFinal(ctx, der.data(), &der_len) == 1) {
            der.resize(der_len);
            const std::string signature = ecdsa_der_to_jose(der.data(), der.size());
            if (!signature.empty()) {
                jwt = signing_input + "." + signature;
            }
        }
    }

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return jwt;
}

auto CoinbaseFeedHandler::build_subscription_messages() -> std::vector<std::string> {
    std::vector<std::string> messages;
    const std::string api_key = coinbase_api_key_from_env();
    const std::string api_secret = coinbase_api_secret_from_env();
    const bool have_credentials = !api_key.empty() && !api_secret.empty();

    auto make_sub = [&](const char* channel, bool include_product_ids) {
        nlohmann::json sub = {{"type", "subscribe"}, {"channel", channel}};
        if (include_product_ids) {
            sub["product_ids"] = nlohmann::json::array({venue_symbols_.coinbase});
        }
        if (have_credentials) {
            const std::string jwt = generate_jwt(api_key, api_secret);
            if (!jwt.empty()) {
                sub["jwt"] = jwt;
            } else {
                LOG_ERROR("[Coinbase] JWT generation failed despite credentials being present — "
                          "sending unauthenticated subscribe (snapshot will likely time out)",
                          "symbol", symbol_.c_str(), "channel", channel);
            }
        }
        messages.push_back(sub.dump());
    };

    make_sub("heartbeats", false);
    make_sub("level2", true);
    return messages;
}

auto CoinbaseFeedHandler::fetch_tick_size() -> Result {
    const std::string url = api_url_ + "/api/v3/brokerage/market/products/" + venue_symbols_.coinbase;
    auto resp = http::get(url);
    if (!resp.ok() || resp.body.empty()) {
        LOG_WARN("[Coinbase] fetch_tick_size failed", "symbol", symbol_.c_str(), "status", resp.status);
        return Result::ERROR_CONNECTION_LOST;
    }
    auto json = nlohmann::json::parse(resp.body, nullptr, false);
    if (json.is_discarded()) {
        LOG_WARN("[Coinbase] fetch_tick_size JSON parse failed", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }
    std::string ts = json.value("quote_increment", std::string(""));
    if (ts.empty()) {
        LOG_WARN("[Coinbase] fetch_tick_size: quote_increment missing", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }
    tick_size_ = tick_from_string(ts);
    LOG_INFO("[Coinbase] Tick size fetched", "symbol", symbol_.c_str(), "tick_size", tick_size_);
    return Result::SUCCESS;
}

void CoinbaseFeedHandler::emit_ops_event() {
    const int64_t ts_ns = http::now_ns();
    char json_buf[256];
    std::snprintf(json_buf, sizeof(json_buf),
                  "{\"event\":\"coinbase_feed_failed\",\"timestamp_ns\":%lld,\"symbol\":\"%s\"}\n",
                  static_cast<long long>(ts_ns), symbol_.c_str());
    std::fprintf(stdout, "[OPS_EVENT] coinbase_feed_failed: {\"symbol\":\"%s\"}\n",
                 symbol_.c_str());
    std::fflush(stdout);
    if (FILE* f = std::fopen("logs/ops_events.jsonl", "a")) {
        std::fputs(json_buf, f);
        std::fclose(f);
    }
}

}
