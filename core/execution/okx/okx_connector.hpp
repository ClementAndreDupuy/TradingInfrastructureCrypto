#pragma once

#include "../common/live_connector_base.hpp"

namespace trading {

class OkxConnector : public LiveConnectorBase {
  public:
    OkxConnector(const std::string& api_key, const std::string& api_secret,
                 const std::string& api_passphrase,
                 const std::string& api_url = "https://www.okx.com")
        : LiveConnectorBase(Exchange::OKX, api_key, api_secret, api_url, {}, api_passphrase) {}

  protected:
    ConnectorResult submit_to_venue(const Order& order, const std::string& idempotency_key,
                                    std::string& venue_order_id) override;
    ConnectorResult cancel_at_venue(const VenueOrderEntry& entry) override;
    ConnectorResult replace_at_venue(const VenueOrderEntry& entry, const Order& replacement,
                                     std::string& new_venue_order_id) override;
    ConnectorResult query_at_venue(const VenueOrderEntry& entry, FillUpdate& status) override;
    ConnectorResult cancel_all_at_venue(const char* symbol) override;
    ConnectorResult fetch_reconciliation_snapshot(ReconciliationSnapshot& snapshot) override;
};

}
