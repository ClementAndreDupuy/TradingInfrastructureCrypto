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

    enum class MismatchClass : uint8_t {
        NONE = 0,
        MISSING_ORDER = 1,
        QTY_DRIFT = 2,
        FILL_GAP = 3,
        BALANCE_DRIFT = 4,
        POSITION_DRIFT = 5,
    };

    enum class DriftAction : uint8_t {
        NONE = 0,
        QUARANTINE_VENUE = 1,
        REQUEST_FILL_REPLAY = 2,
        RISK_HALT_RECOMMENDED = 3,
    };

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
        MismatchClass last_mismatch = MismatchClass::NONE;
        DriftAction last_action = DriftAction::NONE;
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

    bool set_canonical_snapshot(Exchange exchange, const ReconciliationSnapshot& snapshot) {
        const size_t idx = index_for(exchange);
        if (idx == MAX_CONNECTORS)
            return false;
        canonical_snapshots_[idx] = snapshot;
        has_canonical_snapshot_[idx] = true;
        return true;
    }

    ConnectorResult reconcile_on_reconnect() {
        return run_reconciliation_cycle(true);
    }

    ConnectorResult run_periodic_drift_check() {
        return run_reconciliation_cycle(false);
    }

    bool is_quarantined(Exchange exchange) const {
        const VenueState* state = find_state(exchange);
        return state ? state->quarantined : false;
    }

    const VenueState* state_for(Exchange exchange) const { return find_state(exchange); }

  private:
    struct DriftDecision {
        bool mismatch = false;
        MismatchClass mismatch_class = MismatchClass::NONE;
        DriftAction action = DriftAction::NONE;
        const char* reason = "";
    };

    ConnectorResult run_reconciliation_cycle(bool reconnect_phase) {
        for (size_t i = 0; i < connector_count_; ++i) {
            if (states_[i].quarantined)
                continue;

            ReconciliationSnapshot snapshot;
            const ConnectorResult res = connectors_[i]->fetch_reconciliation_snapshot(snapshot);
            if (res != ConnectorResult::OK) {
                quarantine(i, MismatchClass::NONE, DriftAction::QUARANTINE_VENUE,
                           reconnect_phase ? "snapshot fetch failed"
                                           : "periodic snapshot fetch failed");
                return res;
            }

            const DriftDecision decision =
                has_canonical_snapshot_[i] ? evaluate_drift(snapshot, canonical_snapshots_[i])
                                           : evaluate_snapshot_sanity(snapshot);
            if (decision.mismatch) {
                quarantine(i, decision.mismatch_class, decision.action, decision.reason);
                return ConnectorResult::ERROR_UNKNOWN;
            }

            const int64_t ts_ns = now_ns();
            if (reconnect_phase) {
                states_[i].last_reconcile_ts_ns = ts_ns;
                states_[i].mismatch_count = 0;
            } else {
                states_[i].last_drift_check_ts_ns = ts_ns;
            }
            states_[i].last_mismatch = MismatchClass::NONE;
            states_[i].last_action = DriftAction::NONE;
        }
        return ConnectorResult::OK;
    }

    DriftDecision evaluate_snapshot_sanity(const ReconciliationSnapshot& snapshot) const {
        DriftDecision out;

        for (size_t i = 0; i < snapshot.open_orders.size; ++i) {
            const auto& order = snapshot.open_orders.items[i];
            if (order.quantity < 0.0 || order.filled_quantity < 0.0) {
                out.mismatch = true;
                out.action = DriftAction::QUARANTINE_VENUE;
                out.reason = "negative order quantity";
                return out;
            }
            const double remaining = order.quantity - order.filled_quantity;
            if (remaining < -thresholds_.max_order_fill_gap) {
                out.mismatch = true;
                out.action = DriftAction::QUARANTINE_VENUE;
                out.reason = "order overfilled";
                return out;
            }
        }

        for (size_t i = 0; i < snapshot.balances.size; ++i) {
            const auto& balance = snapshot.balances.items[i];
            const double drift = balance.total - balance.available;
            if (drift < -thresholds_.max_balance_drift) {
                out.mismatch = true;
                out.action = DriftAction::QUARANTINE_VENUE;
                out.reason = "balance available exceeds total";
                return out;
            }
        }

        for (size_t i = 0; i < snapshot.positions.size; ++i) {
            const auto& position = snapshot.positions.items[i];
            if (position.avg_entry_price < 0.0) {
                out.mismatch = true;
                out.action = DriftAction::QUARANTINE_VENUE;
                out.reason = "negative average entry price";
                return out;
            }
            if (position.quantity > 0.0 &&
                position.avg_entry_price <= thresholds_.max_position_drift) {
                out.mismatch = true;
                out.action = DriftAction::QUARANTINE_VENUE;
                out.reason = "positive position with invalid entry";
                return out;
            }
        }

        return out;
    }

    DriftDecision evaluate_drift(const ReconciliationSnapshot& venue,
                                 const ReconciliationSnapshot& canonical) const {
        DriftDecision out = evaluate_snapshot_sanity(venue);
        if (out.mismatch)
            return out;

        for (size_t i = 0; i < canonical.open_orders.size; ++i) {
            const ReconciledOrder& internal_order = canonical.open_orders.items[i];
            const ReconciledOrder* venue_order = find_order(venue, internal_order);
            if (!venue_order) {
                return mismatch(MismatchClass::MISSING_ORDER, DriftAction::QUARANTINE_VENUE,
                                "missing_order");
            }
            if (drift(venue_order->quantity, internal_order.quantity) >
                    thresholds_.max_order_fill_gap ||
                drift(venue_order->filled_quantity, internal_order.filled_quantity) >
                    thresholds_.max_order_fill_gap) {
                return mismatch(MismatchClass::QTY_DRIFT, DriftAction::QUARANTINE_VENUE,
                                "qty_drift");
            }
        }

        for (size_t i = 0; i < venue.open_orders.size; ++i) {
            const ReconciledOrder& venue_order = venue.open_orders.items[i];
            if (!find_order(canonical, venue_order)) {
                return mismatch(MismatchClass::MISSING_ORDER, DriftAction::QUARANTINE_VENUE,
                                "missing_order");
            }
        }

        if (fill_gap_detected(venue, canonical)) {
            return mismatch(MismatchClass::FILL_GAP, DriftAction::REQUEST_FILL_REPLAY,
                            "fill_gap");
        }

        for (size_t i = 0; i < canonical.balances.size; ++i) {
            const ReconciledBalance& internal_balance = canonical.balances.items[i];
            const ReconciledBalance* venue_balance = find_balance(venue, internal_balance.asset);
            if (!venue_balance ||
                drift(venue_balance->total, internal_balance.total) > thresholds_.max_balance_drift ||
                drift(venue_balance->available, internal_balance.available) >
                    thresholds_.max_balance_drift) {
                return mismatch(MismatchClass::BALANCE_DRIFT, DriftAction::RISK_HALT_RECOMMENDED,
                                "balance_drift");
            }
        }

        for (size_t i = 0; i < canonical.positions.size; ++i) {
            const ReconciledPosition& internal_position = canonical.positions.items[i];
            const ReconciledPosition* venue_position =
                find_position(venue, internal_position.symbol);
            if (!venue_position ||
                drift(venue_position->quantity, internal_position.quantity) >
                    thresholds_.max_position_drift ||
                drift(venue_position->avg_entry_price, internal_position.avg_entry_price) >
                    thresholds_.max_position_drift) {
                return mismatch(MismatchClass::POSITION_DRIFT,
                                DriftAction::RISK_HALT_RECOMMENDED, "position_drift");
            }
        }

        return out;
    }

    static DriftDecision mismatch(MismatchClass mismatch_class, DriftAction action,
                                  const char* reason) {
        DriftDecision out;
        out.mismatch = true;
        out.mismatch_class = mismatch_class;
        out.action = action;
        out.reason = reason;
        return out;
    }

    void quarantine(size_t idx, MismatchClass mismatch_class, DriftAction action,
                    const char* reason) {
        states_[idx].quarantined = true;
        ++states_[idx].mismatch_count;
        states_[idx].last_mismatch = mismatch_class;
        states_[idx].last_action = action;
        LOG_ERROR("reconciliation quarantine", "exchange",
                  exchange_to_string(states_[idx].exchange), "reason", reason, "action",
                  static_cast<int>(action));
    }

    static double drift(double a, double b) noexcept { return std::fabs(a - b); }

    static bool str_eq(const char* lhs, const char* rhs) noexcept {
        return std::strncmp(lhs, rhs, 16) == 0;
    }

    static bool venue_id_eq(const char* lhs, const char* rhs) noexcept {
        return std::strncmp(lhs, rhs, 64) == 0;
    }

    static bool is_empty_venue_id(const char* venue_order_id) noexcept {
        return venue_order_id[0] == '\0';
    }

    static const ReconciledOrder* find_order(const ReconciliationSnapshot& snapshot,
                                             const ReconciledOrder& key) noexcept {
        for (size_t i = 0; i < snapshot.open_orders.size; ++i) {
            const ReconciledOrder& candidate = snapshot.open_orders.items[i];
            if (candidate.client_order_id != 0 && key.client_order_id != 0 &&
                candidate.client_order_id == key.client_order_id) {
                return &candidate;
            }
            if (!is_empty_venue_id(candidate.venue_order_id) &&
                !is_empty_venue_id(key.venue_order_id) &&
                venue_id_eq(candidate.venue_order_id, key.venue_order_id)) {
                return &candidate;
            }
        }
        return nullptr;
    }

    static const ReconciledBalance* find_balance(const ReconciliationSnapshot& snapshot,
                                                 const char* asset) noexcept {
        for (size_t i = 0; i < snapshot.balances.size; ++i) {
            const ReconciledBalance& candidate = snapshot.balances.items[i];
            if (str_eq(candidate.asset, asset))
                return &candidate;
        }
        return nullptr;
    }

    static const ReconciledPosition* find_position(const ReconciliationSnapshot& snapshot,
                                                   const char* symbol) noexcept {
        for (size_t i = 0; i < snapshot.positions.size; ++i) {
            const ReconciledPosition& candidate = snapshot.positions.items[i];
            if (str_eq(candidate.symbol, symbol))
                return &candidate;
        }
        return nullptr;
    }

    double aggregate_fill_qty_for_order(const ReconciliationSnapshot& snapshot,
                                        const ReconciledOrder& order) const noexcept {
        double qty = 0.0;
        for (size_t i = 0; i < snapshot.fills.size; ++i) {
            const ReconciledFill& fill = snapshot.fills.items[i];
            if (order.client_order_id != 0 && fill.client_order_id == order.client_order_id) {
                qty += fill.quantity;
                continue;
            }
            if (!is_empty_venue_id(order.venue_order_id) &&
                venue_id_eq(fill.venue_order_id, order.venue_order_id)) {
                qty += fill.quantity;
            }
        }
        return qty;
    }

    bool fill_gap_detected(const ReconciliationSnapshot& venue,
                           const ReconciliationSnapshot& canonical) const noexcept {
        for (size_t i = 0; i < canonical.open_orders.size; ++i) {
            const ReconciledOrder& internal_order = canonical.open_orders.items[i];
            const double internal_qty = aggregate_fill_qty_for_order(canonical, internal_order);
            const double venue_qty = aggregate_fill_qty_for_order(venue, internal_order);
            if (drift(venue_qty, internal_qty) > thresholds_.max_order_fill_gap)
                return true;
        }
        return false;
    }

    size_t index_for(Exchange exchange) const noexcept {
        for (size_t i = 0; i < connector_count_; ++i) {
            if (states_[i].exchange == exchange)
                return i;
        }
        return MAX_CONNECTORS;
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
    std::array<ReconciliationSnapshot, MAX_CONNECTORS> canonical_snapshots_{};
    std::array<bool, MAX_CONNECTORS> has_canonical_snapshot_{};
    size_t connector_count_ = 0;
};

} // namespace trading
