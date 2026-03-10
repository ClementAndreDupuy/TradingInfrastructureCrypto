#pragma once

// Binance execution connector.
//
// Order submission:  REST POST /api/v3/order    (HMAC-SHA256 signed)
// Order cancellation: REST DELETE /api/v3/order
// Fill updates:      User Data Stream WebSocket (executionReport events)
//                    Requires listenKey obtained via POST /api/v3/userDataStream.
//                    listenKey kept alive every 30 min via PUT.
//
// Credentials via environment variables (or constructor):
//   BINANCE_API_KEY
//   BINANCE_API_SECRET

#include "exchange_connector.hpp"
#include <string>
#include <array>
#include <atomic>

namespace trading {

class BinanceConnector : public ExchangeConnector {
public:
    explicit BinanceConnector(
        const std::string& api_key    = "",
        const std::string& api_secret = "",
        const std::string& api_url    = "https://api.binance.com",
        const std::string& ws_url     = "wss://stream.binance.com:9443/ws"
    );

    ~BinanceConnector() override;

    static std::string get_api_key_from_env();
    static std::string get_api_secret_from_env();

    ConnectorResult connect()    override;
    void            disconnect() override;

    ConnectorResult submit_order(const Order& order)       override;
    ConnectorResult cancel_order(uint64_t client_order_id) override;
    ConnectorResult cancel_all(const char* symbol)         override;
    ConnectorResult reconcile()                            override;

    bool     is_connected() const override;
    Exchange exchange_id()  const override { return Exchange::BINANCE; }

    // Process a raw WebSocket message (executionReport). Exposed for testing.
    void process_ws_message(const std::string& message);

    // WS endpoint for the user data stream: {ws_url}/{listenKey}
    std::string listen_key() const { return listen_key_; }

private:
    std::string api_key_;
    std::string api_secret_;
    std::string api_url_;
    std::string ws_url_;
    std::string listen_key_;

    std::atomic<bool>    connected_{false};
    std::atomic<int64_t> last_keepalive_ns_{0};

    static constexpr int64_t KEEPALIVE_INTERVAL_NS = 30LL * 60 * 1'000'000'000LL;

    // Compact symbol lookup for cancel_order (pre-allocated, no heap).
    struct TrackedOrder { uint64_t client_id; char symbol[16]; };
    static constexpr size_t MAX_TRACKED = 512;
    std::array<TrackedOrder, MAX_TRACKED> tracked_;
    std::atomic<uint32_t>                tracked_count_{0};

    void        track_order(uint64_t client_id, const char* symbol) noexcept;
    const char* find_tracked_symbol(uint64_t client_id) const noexcept;

    ConnectorResult get_listen_key();
    ConnectorResult keepalive_listen_key();

    std::string build_signed_params(const std::string& params) const;
    std::string hmac_sha256_hex(const std::string& data) const;
    int64_t     timestamp_ms() const;
    int64_t     now_ns() const;

    std::string http_get(const std::string& url) const;
    std::string http_post(const std::string& url, const std::string& body) const;
    std::string http_put(const std::string& url, const std::string& body) const;
    std::string http_delete(const std::string& url) const;

    void on_execution_report(const std::string& msg);

    std::string parse_string(const std::string& json, const std::string& key) const;
    double      parse_double(const std::string& json, const std::string& key) const;
    int64_t     parse_int64(const std::string& json, const std::string& key) const;

    static OrderState map_order_status(const std::string& status) noexcept;
};

}  // namespace trading
