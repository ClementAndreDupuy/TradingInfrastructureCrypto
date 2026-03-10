#pragma once

// Kraken execution connector.
//
// Order submission:    REST POST /0/private/AddOrder   (HMAC-SHA512 signed)
// Order cancellation: REST POST /0/private/CancelOrder
// Fill updates:       Private WebSocket v2 "executions" channel
//                     Auth token obtained via POST /0/private/GetWebSocketsToken.
//
// Kraken HMAC-SHA512 signing scheme:
//   nonce       = monotonically increasing integer (ms timestamp)
//   post_data   = nonce=...&param=...
//   API-Sign    = Base64(HMAC-SHA512(path + SHA256(nonce + post_data),
//                                   Base64Decode(api_secret)))
//
// Credentials via environment variables (or constructor):
//   KRAKEN_API_KEY
//   KRAKEN_API_SECRET

#include "exchange_connector.hpp"
#include <string>
#include <array>
#include <atomic>
#include <vector>

namespace trading {

class KrakenConnector : public ExchangeConnector {
public:
    explicit KrakenConnector(
        const std::string& api_key    = "",
        const std::string& api_secret = "",
        const std::string& api_url    = "https://api.kraken.com",
        const std::string& ws_url     = "wss://ws-auth.kraken.com/v2"
    );

    ~KrakenConnector() override;

    static std::string get_api_key_from_env();
    static std::string get_api_secret_from_env();

    ConnectorResult connect()    override;
    void            disconnect() override;

    ConnectorResult submit_order(const Order& order)       override;
    ConnectorResult cancel_order(uint64_t client_order_id) override;
    ConnectorResult cancel_all(const char* symbol)         override;
    ConnectorResult reconcile()                            override;

    bool     is_connected() const override;
    Exchange exchange_id()  const override { return Exchange::KRAKEN; }

    // Process a raw WebSocket message from the executions channel. Exposed for testing.
    void process_ws_message(const std::string& message);

    // WebSocket URL for caller's event loop to connect to.
    std::string ws_url()    const { return ws_url_; }
    std::string ws_token()  const { return ws_token_; }

private:
    std::string api_key_;
    std::string api_secret_;
    std::string api_url_;
    std::string ws_url_;
    std::string ws_token_;

    std::atomic<bool>    connected_{false};
    std::atomic<uint64_t> next_nonce_{0};

    // Kraken client order IDs are uint64 converted to string.
    // We also store the Kraken txid (exchange_order_id) for cancel by txid.
    struct TrackedOrder { uint64_t client_id; char txid[64]; };
    static constexpr size_t MAX_TRACKED = 512;
    std::array<TrackedOrder, MAX_TRACKED> tracked_;
    std::atomic<uint32_t>                tracked_count_{0};

    void        track_order(uint64_t client_id, const char* txid) noexcept;
    const char* find_txid(uint64_t client_id) const noexcept;

    ConnectorResult get_ws_token();

    // Signing helpers
    std::string kraken_sign(const std::string& path,
                            const std::string& post_data,
                            const std::string& nonce) const;
    std::string base64_encode(const unsigned char* data, size_t len) const;
    std::vector<unsigned char> base64_decode(const std::string& encoded) const;

    std::string generate_nonce();
    int64_t     now_ns() const;

    std::string http_post_private(const std::string& path,
                                  const std::string& post_data) const;
    std::string http_get(const std::string& url) const;

    void on_execution_update(const std::string& msg);

    std::string parse_string(const std::string& json, const std::string& key) const;
    double      parse_double(const std::string& json, const std::string& key) const;
    int64_t     parse_int64(const std::string& json, const std::string& key) const;

    // Map Kraken order_status to OrderState
    static OrderState map_order_status(const std::string& status) noexcept;
    // Map Kraken exec_type to OrderState (for partial fills, etc.)
    static OrderState map_exec_type(const std::string& exec_type) noexcept;
};

}  // namespace trading
