#pragma once

#include "../common/connectors/live_connector_base.hpp"

namespace trading {
    class KrakenConnector : public LiveConnectorBase {
    public:
        KrakenConnector(const std::string &api_key, const std::string &api_secret,
                        const std::string &api_url, RetryPolicy retry_policy = {})
            : LiveConnectorBase(Exchange::KRAKEN, api_key, api_secret, api_url, retry_policy, "") {
        }

    protected:
        ConnectorResult submit_to_venue(const Order &order, const std::string &idempotency_key,
                                        std::string &venue_order_id) override;

        ConnectorResult cancel_at_venue(const VenueOrderEntry &entry) override;

        ConnectorResult replace_at_venue(const VenueOrderEntry &entry, const Order &replacement,
                                         std::string &new_venue_order_id) override;

        ConnectorResult query_at_venue(const VenueOrderEntry &entry, FillUpdate &status) override;

        ConnectorResult cancel_all_at_venue(const char *symbol) override;

        ConnectorResult fetch_reconciliation_snapshot(ReconciliationSnapshot &snapshot) override;
    };
}
