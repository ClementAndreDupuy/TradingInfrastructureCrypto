#pragma once

#include "../common/logging.hpp"
#include "live_connector_base.hpp"
#include "reconciliation_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace trading {

class ReconciliationService {
  public:
    static constexpr size_t MAX_CONNECTORS = 4;

    struct DriftThresholds {
        double max_balance_drift = 1e-6;
        double max_position_drift = 1e-6;
        double max_order_fill_gap = 1e-6;
    };

    struct VenueState {
        Exchange exchange = Exchange::UNKNOWN;
        bool registered = false;
        bool quarantined = false;
        int64_t last_reconcile_ts_ns = 0;
        int64_t last_drift_check_ts_ns = 0;
        uint32_t mismatch_count = 0;
    };

    ReconciliationService() = default;
    explicit ReconciliationService(const DriftThresholds& thresholds) : thresholds_(thresholds) {}

    bool register_connector(LiveConnectorBase& connector) {
        for (size_t i = 0; i < connector_count_; ++i) {
            if (connectors_[i] == &connector)
                return true;
        }
        if (connector_count_ >= MAX_CONNECTORS)
            return false;

        connectors_[connector_count_] = &connector;
        states_[connector_count_].exchange = connector.exchange_id();
        states_[connector_count_].registered = true;
        ++connector_count_;
        return true;
    }

    ConnectorResult reconcile_on_reconnect() {
        for (size_t i = 0; i < connector_count_; ++i) {
            if (states_[i].quarantined)
                continue;

            ReconciliationSnapshot snapshot;
            const ConnectorResult res = connectors_[i]->fetch_reconciliation_snapshot(snapshot);
            if (res != ConnectorResult::OK) {
                quarantine(i, "snapshot fetch failed");
                return res;
            }

            const DriftDecision decision = evaluate_snapshot(snapshot);
            if (decision.mismatch) {
                quarantine(i, decision.reason);
                return ConnectorResult::ERROR_UNKNOWN;
            }

            states_[i].last_reconcile_ts_ns = now_ns();
            states_[i].mismatch_count = 0;
        }
        return ConnectorResult::OK;
    }

    ConnectorResult run_periodic_drift_check() {
        for (size_t i = 0; i < connector_count_; ++i) {
            if (states_[i].quarantined)
                continue;

            ReconciliationSnapshot snapshot;
            const ConnectorResult res = connectors_[i]->fetch_reconciliation_snapshot(snapshot);
            if (res != ConnectorResult::OK) {
                quarantine(i, "periodic snapshot fetch failed");
                return res;
            }

            const DriftDecision decision = evaluate_snapshot(snapshot);
            if (decision.mismatch) {
                quarantine(i, decision.reason);
                return ConnectorResult::ERROR_UNKNOWN;
            }

            states_[i].last_drift_check_ts_ns = now_ns();
        }
        return ConnectorResult::OK;
    }

    bool is_quarantined(Exchange exchange) const {
        const VenueState* state = find_state(exchange);
        return state ? state->quarantined : false;
    }

    const VenueState* state_for(Exchange exchange) const { return find_state(exchange); }

  private:
    struct DriftDecision {
        bool mismatch = false;
        const char* reason = "";
    };

    DriftDecision evaluate_snapshot(const ReconciliationSnapshot& snapshot) const {
        DriftDecision out;

        for (size_t i = 0; i < snapshot.open_orders.size; ++i) {
            const auto& order = snapshot.open_orders.items[i];
            if (order.quantity < 0.0 || order.filled_quantity < 0.0) {
                out.mismatch = true;
                out.reason = "negative order quantity";
                return out;
            }
            const double remaining = order.quantity - order.filled_quantity;
            if (remaining < -thresholds_.max_order_fill_gap) {
                out.mismatch = true;
                out.reason = "order overfilled";
                return out;
            }
        }

        for (size_t i = 0; i < snapshot.balances.size; ++i) {
            const auto& balance = snapshot.balances.items[i];
            const double drift = balance.total - balance.available;
            if (drift < -thresholds_.max_balance_drift) {
                out.mismatch = true;
                out.reason = "balance available exceeds total";
                return out;
            }
        }

        for (size_t i = 0; i < snapshot.positions.size; ++i) {
            const auto& position = snapshot.positions.items[i];
            if (position.avg_entry_price < 0.0) {
                out.mismatch = true;
                out.reason = "negative average entry price";
                return out;
            }
            if (position.quantity > 0.0 &&
                position.avg_entry_price <= thresholds_.max_position_drift) {
                out.mismatch = true;
                out.reason = "positive position with invalid entry";
                return out;
            }
        }

        return out;
    }

    void quarantine(size_t idx, const char* reason) {
        states_[idx].quarantined = true;
        ++states_[idx].mismatch_count;
        LOG_ERROR("reconciliation quarantine", "exchange",
                  exchange_to_string(states_[idx].exchange), "reason", reason);
    }

    const VenueState* find_state(Exchange exchange) const {
        for (size_t i = 0; i < connector_count_; ++i) {
            if (states_[i].registered && states_[i].exchange == exchange)
                return &states_[i];
        }
        return nullptr;
    }

    static int64_t now_ns() noexcept { return http::now_ns(); }

    DriftThresholds thresholds_;
    std::array<LiveConnectorBase*, MAX_CONNECTORS> connectors_{};
    std::array<VenueState, MAX_CONNECTORS> states_{};
    size_t connector_count_ = 0;
};

} // namespace trading
