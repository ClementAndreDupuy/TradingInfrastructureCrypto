#pragma once

#include "../common/logging.hpp"
#include "live_connector_base.hpp"
#include "reconciliation_types.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace trading {

class ReconciliationService {
  public:
    static constexpr size_t MAX_CONNECTORS = 4;

    struct DriftThresholds {
        double max_balance_drift = 1e-6;
        double max_position_drift = 1e-6;
        double max_order_fill_gap = 1e-6;
        double max_order_qty_drift = 1e-6;
        double max_order_price_drift = 1e-6;
    };

    struct ExpectedState {
        ReconciliationSnapshot snapshot;
        bool configured = false;
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

    explicit ReconciliationService(DriftThresholds thresholds) : thresholds_(thresholds) {}

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

    bool set_expected_state(Exchange exchange, const ReconciliationSnapshot& expected) {
        const size_t idx = find_index(exchange);
        if (idx == MAX_CONNECTORS)
            return false;
        expected_[idx].snapshot = expected;
        expected_[idx].configured = true;
        return true;
    }

    ConnectorResult reconcile_on_reconnect() { return run_check(true); }

    ConnectorResult run_periodic_drift_check() { return run_check(false); }

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

    ConnectorResult run_check(bool reconnect) {
        for (size_t i = 0; i < connector_count_; ++i) {
            if (states_[i].quarantined)
                continue;

            ReconciliationSnapshot snapshot;
            const ConnectorResult res = connectors_[i]->fetch_reconciliation_snapshot(snapshot);
            if (res != ConnectorResult::OK) {
                quarantine(i,
                           reconnect ? "snapshot fetch failed" : "periodic snapshot fetch failed");
                return res;
            }

            DriftDecision decision = evaluate_snapshot(snapshot);
            if (!decision.mismatch && expected_[i].configured)
                decision = evaluate_vs_expected(snapshot, expected_[i].snapshot);

            if (decision.mismatch) {
                quarantine(i, decision.reason);
                return ConnectorResult::ERROR_UNKNOWN;
            }

            const int64_t ts = now_ns();
            if (reconnect) {
                states_[i].last_reconcile_ts_ns = ts;
                states_[i].mismatch_count = 0;
            } else {
                states_[i].last_drift_check_ts_ns = ts;
            }
        }
        return ConnectorResult::OK;
    }

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
            if (balance.total < 0.0 || balance.available < 0.0) {
                out.mismatch = true;
                out.reason = "negative balance";
                return out;
            }
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

        for (size_t i = 0; i < snapshot.fills.size; ++i) {
            const auto& fill = snapshot.fills.items[i];
            if (fill.quantity <= 0.0 || fill.price <= 0.0 || fill.exchange_ts_ns <= 0) {
                out.mismatch = true;
                out.reason = "invalid fill record";
                return out;
            }
        }

        return out;
    }

    DriftDecision evaluate_vs_expected(const ReconciliationSnapshot& actual,
                                       const ReconciliationSnapshot& expected) const {
        DriftDecision out;
        if (actual.open_orders.size != expected.open_orders.size) {
            out.mismatch = true;
            out.reason = "open order count drift";
            return out;
        }

        for (size_t i = 0; i < expected.open_orders.size; ++i) {
            const auto* matched = find_order(actual, expected.open_orders.items[i].venue_order_id);
            if (!matched) {
                out.mismatch = true;
                out.reason = "missing expected order";
                return out;
            }
            if (std::fabs(matched->quantity - expected.open_orders.items[i].quantity) >
                thresholds_.max_order_qty_drift) {
                out.mismatch = true;
                out.reason = "order quantity drift";
                return out;
            }
            if (std::fabs(matched->price - expected.open_orders.items[i].price) >
                thresholds_.max_order_price_drift) {
                out.mismatch = true;
                out.reason = "order price drift";
                return out;
            }
        }

        for (size_t i = 0; i < expected.balances.size; ++i) {
            const auto* matched = find_balance(actual, expected.balances.items[i].asset);
            if (!matched) {
                out.mismatch = true;
                out.reason = "missing expected balance";
                return out;
            }
            if (std::fabs(matched->total - expected.balances.items[i].total) >
                thresholds_.max_balance_drift) {
                out.mismatch = true;
                out.reason = "balance total drift";
                return out;
            }
        }

        for (size_t i = 0; i < expected.positions.size; ++i) {
            const auto* matched = find_position(actual, expected.positions.items[i].symbol);
            if (!matched) {
                out.mismatch = true;
                out.reason = "missing expected position";
                return out;
            }
            if (std::fabs(matched->quantity - expected.positions.items[i].quantity) >
                thresholds_.max_position_drift) {
                out.mismatch = true;
                out.reason = "position quantity drift";
                return out;
            }
        }

        return out;
    }

    static const ReconciledOrder* find_order(const ReconciliationSnapshot& snapshot,
                                             const char* venue_order_id) {
        for (size_t i = 0; i < snapshot.open_orders.size; ++i) {
            if (std::strcmp(snapshot.open_orders.items[i].venue_order_id, venue_order_id) == 0)
                return &snapshot.open_orders.items[i];
        }
        return nullptr;
    }

    static const ReconciledBalance* find_balance(const ReconciliationSnapshot& snapshot,
                                                 const char* asset) {
        for (size_t i = 0; i < snapshot.balances.size; ++i) {
            if (std::strcmp(snapshot.balances.items[i].asset, asset) == 0)
                return &snapshot.balances.items[i];
        }
        return nullptr;
    }

    static const ReconciledPosition* find_position(const ReconciliationSnapshot& snapshot,
                                                   const char* symbol) {
        for (size_t i = 0; i < snapshot.positions.size; ++i) {
            if (std::strcmp(snapshot.positions.items[i].symbol, symbol) == 0)
                return &snapshot.positions.items[i];
        }
        return nullptr;
    }

    void quarantine(size_t idx, const char* reason) {
        states_[idx].quarantined = true;
        ++states_[idx].mismatch_count;
        LOG_ERROR("reconciliation quarantine", "exchange",
                  exchange_to_string(states_[idx].exchange), "reason", reason);
    }

    size_t find_index(Exchange exchange) const {
        for (size_t i = 0; i < connector_count_; ++i) {
            if (states_[i].registered && states_[i].exchange == exchange)
                return i;
        }
        return MAX_CONNECTORS;
    }

    const VenueState* find_state(Exchange exchange) const {
        const size_t idx = find_index(exchange);
        if (idx == MAX_CONNECTORS)
            return nullptr;
        return &states_[idx];
    }

    static int64_t now_ns() noexcept { return http::now_ns(); }

    DriftThresholds thresholds_;
    std::array<LiveConnectorBase*, MAX_CONNECTORS> connectors_{};
    std::array<VenueState, MAX_CONNECTORS> states_{};
    std::array<ExpectedState, MAX_CONNECTORS> expected_{};
    size_t connector_count_ = 0;
};

} // namespace trading
