#pragma once

#include "../live_connector_base.hpp"

namespace trading {

class CoinbaseConnector : public LiveConnectorBase {
  public:
    CoinbaseConnector(const std::string& api_key, const std::string& api_secret,
                      const std::string& api_url = "https://api.coinbase.com")
        : LiveConnectorBase(Exchange::COINBASE, api_key, api_secret, api_url) {}

  protected:
    ConnectorResult submit_to_venue(const Order& order, const std::string& idempotency_key,
                                    std::string& venue_order_id) override;
    ConnectorResult cancel_at_venue(const VenueOrderEntry& entry) override;
    ConnectorResult replace_at_venue(const VenueOrderEntry& entry, const Order& replacement,
                                     std::string& new_venue_order_id) override;
    ConnectorResult query_at_venue(const VenueOrderEntry& entry, FillUpdate& status) override;
    ConnectorResult cancel_all_at_venue(const char* symbol) override;
};

} // namespace trading
