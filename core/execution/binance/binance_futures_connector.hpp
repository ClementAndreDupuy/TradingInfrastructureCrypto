#pragma once

#include "../common/connectors/live_connector_base.hpp"

#include <unordered_map>

namespace trading {
    struct BinanceFuturesSymbolFilters {
        bool valid = false;
        double min_price = 0.0;
        double max_price = 0.0;
        double tick_size = 0.0;
        double min_qty = 0.0;
        double max_qty = 0.0;
        double step_size = 0.0;
        double min_market_qty = 0.0;
        double max_market_qty = 0.0;
        double market_step_size = 0.0;
        double min_notional = 0.0;
        double trigger_protect = 0.0;
    };

    class BinanceFuturesConnector : public LiveConnectorBase {
    public:
        BinanceFuturesConnector(const std::string &api_key, const std::string &api_secret,
                                const std::string &api_url, uint32_t recv_window_ms = 5000,
                                RetryPolicy retry_policy = {})
            : LiveConnectorBase(Exchange::BINANCE, api_key, api_secret, api_url, retry_policy, ""),
              recv_window_ms_(recv_window_ms) {
        }
        ConnectorResult connect() override;

        void set_mark_price(double price) noexcept { mark_price_ = price; }
        [[nodiscard]] double mark_price() const noexcept { return mark_price_; }

    protected:
        ConnectorResult submit_to_venue(const Order &order, const std::string &idempotency_key,
                                        std::string &venue_order_id) override;
        ConnectorResult cancel_at_venue(const VenueOrderEntry &entry) override;
        ConnectorResult replace_at_venue(const VenueOrderEntry &entry, const Order &replacement,
                                         std::string &new_venue_order_id) override;
        ConnectorResult query_at_venue(const VenueOrderEntry &entry, FillUpdate &status) override;
        ConnectorResult cancel_all_at_venue(const char *symbol) override;
        ConnectorResult fetch_reconciliation_snapshot(ReconciliationSnapshot &snapshot) override;

    private:
        ConnectorResult get_symbol_filters(const std::string &symbol, BinanceFuturesSymbolFilters &filters);

        double mark_price_ = 0.0;
        uint32_t recv_window_ms_;
        std::unordered_map<std::string, BinanceFuturesSymbolFilters> symbol_filters_;
    };
}
