#pragma once

#include "../live_connector_base.hpp"

namespace trading {

class BinanceConnector : public LiveConnectorBase {
  public:
    BinanceConnector(const std::string& api_key, const std::string& api_secret,
                     const std::string& api_url = "https://api.binance.com")
        : LiveConnectorBase(Exchange::BINANCE, api_key, api_secret, api_url) {}

  protected:
    ConnectorResult submit_to_venue(const Order& order, const std::string& idempotency_key,
                                    std::string& venue_order_id) override;
    ConnectorResult cancel_at_venue(const VenueOrderEntry& entry) override;
    ConnectorResult cancel_all_at_venue(const char* symbol) override;
};

} // namespace trading
