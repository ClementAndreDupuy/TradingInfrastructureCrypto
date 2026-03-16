#pragma once

#include "../common/logging.hpp"
#include "live_connector_base.hpp"
#include "reconciliation_types.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string_view>
#include <utility>

namespace trading {

class ReconciliationService {
  public:
    static constexpr size_t MAX_CONNECTORS = 4;
    static constexpr size_t MAX_FILL_KEYS = 1024;
    using CanonicalSnapshotFetcher = std::function<bool(ReconciliationSnapshot&)>;

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
        CANCEL_ALL_ORDERS = 4,
    };

    enum class SeverityLevel : uint8_t {
        INFO = 0,
        WARNING = 1,
        CRITICAL = 2,
    };

    struct RemediationPolicy {
        uint32_t order_drift_retry_budget = 1;
        uint32_t fill_gap_retry_budget = 2;
        uint32_t snapshot_failure_retry_budget = 0;
    };

    struct DriftThresholds {
        double max_balance_drift = 1e-6;
        double max_position_drift = 1e-6;
        double max_order_fill_gap = 1e-6;
        double max_fill_notional_drift = 1e-6;
        double max_fill_fee_drift = 1e-6;
    };

    struct VenueState {
        Exchange exchange = Exchange::UNKNOWN;
        bool registered = false;
        bool quarantined = false;
        int64_t last_reconcile_ts_ns = 0;
        int64_t last_drift_check_ts_ns = 0;
        uint32_t mismatch_count = 0;
        uint32_t order_drift_retries = 0;
        uint32_t fill_gap_retries = 0;
        uint32_t snapshot_failure_retries = 0;
        uint32_t fill_replay_requests = 0;
        MismatchClass last_mismatch = MismatchClass::NONE;
        DriftAction last_action = DriftAction::NONE;
        SeverityLevel last_severity = SeverityLevel::INFO;
    };

    struct ReconciliationIncident {
        int64_t ts_ns = 0;
        Exchange exchange = Exchange::UNKNOWN;
        MismatchClass mismatch_class = MismatchClass::NONE;
        DriftAction action = DriftAction::NONE;
        SeverityLevel severity = SeverityLevel::INFO;
        bool reconnect_phase = false;
        uint32_t retry_count = 0;
        char reason[48] = {};
    };

    using RemediationHook = std::function<void(Exchange, MismatchClass, std::string_view)>;

    static constexpr size_t MAX_INCIDENT_TRAIL = 256;

    ReconciliationService() = default;
    explicit ReconciliationService(const DriftThresholds& thresholds) : thresholds_(thresholds) {}

    ReconciliationService(const DriftThresholds& thresholds, const RemediationPolicy& policy)
        : thresholds_(thresholds), policy_(policy) {}

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

    bool set_canonical_snapshot_fetcher(Exchange exchange, CanonicalSnapshotFetcher fetcher) {
        const size_t idx = index_for(exchange);
        if (idx == MAX_CONNECTORS)
            return false;
        canonical_snapshot_fetchers_[idx] = std::move(fetcher);
        return true;
    }

    ConnectorResult reconcile_on_reconnect() { return run_reconciliation_cycle(true); }

    ConnectorResult run_periodic_drift_check() { return run_reconciliation_cycle(false); }

    bool is_quarantined(Exchange exchange) const {
        const VenueState* state = find_state(exchange);
        return state ? state->quarantined : false;
    }

    const VenueState* state_for(Exchange exchange) const { return find_state(exchange); }

    void set_cancel_all_hook(RemediationHook hook) { cancel_all_hook_ = std::move(hook); }

    void set_risk_halt_hook(RemediationHook hook) { risk_halt_hook_ = std::move(hook); }

    const ReconciliationIncident* incident_trail(size_t& count) const noexcept {
        count = incident_count_;
        return incident_trail_.data();
    }

    uint32_t dropped_incident_count() const noexcept { return dropped_incident_count_; }

  private:
    struct DriftDecision {
        bool mismatch = false;
        MismatchClass mismatch_class = MismatchClass::NONE;
        DriftAction action = DriftAction::NONE;
        SeverityLevel severity = SeverityLevel::INFO;
        uint32_t retry_count = 0;
        bool quarantine = false;
        const char* reason = "";
    };

    struct FillLedger {
        double cum_qty = 0.0;
        double cum_notional = 0.0;
        double cum_fee = 0.0;
        uint32_t unique_fill_count = 0;
        bool overflow = false;
    };

    struct FillIdentity {
        uint64_t client_order_id = 0;
        char venue_order_id[64] = {};
        char venue_trade_id[64] = {};
        int64_t exchange_ts_ns = 0;
        double quantity = 0.0;
        double price = 0.0;
        Exchange exchange = Exchange::UNKNOWN;
    };

    ConnectorResult run_reconciliation_cycle(bool reconnect_phase) {
        for (size_t i = 0; i < connector_count_; ++i) {
            if (states_[i].quarantined)
                continue;

            ReconciliationSnapshot snapshot;
            const ConnectorResult res = connectors_[i]->fetch_reconciliation_snapshot(snapshot);
            if (res != ConnectorResult::OK) {
                const DriftDecision decision =
                    classify_snapshot_failure(states_[i], reconnect_phase, res);
                apply_decision(i, decision, reconnect_phase);
                return res;
            }
            reset_snapshot_failures(i);

            ReconciliationSnapshot canonical_snapshot;
            const ReconciliationSnapshot* canonical = nullptr;
            if (canonical_snapshot_fetchers_[i]) {
                canonical_snapshot.clear();
                if (!canonical_snapshot_fetchers_[i](canonical_snapshot)) {
                    const DriftDecision decision = mismatch(
                        MismatchClass::NONE, DriftAction::QUARANTINE_VENUE,
                        SeverityLevel::CRITICAL, "canonical snapshot fetch failed", true);
                    apply_decision(i, decision, reconnect_phase);
                    return ConnectorResult::ERROR_UNKNOWN;
                }
                canonical = &canonical_snapshot;
            } else if (has_canonical_snapshot_[i]) {
                canonical = &canonical_snapshots_[i];
            }

            const DriftDecision decision = canonical ? evaluate_drift(snapshot, *canonical)
                                                     : evaluate_snapshot_sanity(snapshot);
            if (decision.mismatch) {
                apply_decision(i, decision, reconnect_phase);
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
            states_[i].last_severity = SeverityLevel::INFO;
            reset_drift_retries(i);
        }
        return ConnectorResult::OK;
    }

    DriftDecision classify_snapshot_failure(VenueState& state, bool reconnect_phase,
                                            ConnectorResult res) const {
        DriftDecision out;
        out.mismatch = true;
        ++state.snapshot_failure_retries;
        out.retry_count = state.snapshot_failure_retries;
        out.reason = reconnect_phase ? "snapshot fetch failed" : "periodic snapshot fetch failed";
        if (res == ConnectorResult::AUTH_FAILED ||
            out.retry_count > policy_.snapshot_failure_retry_budget) {
            out.action = DriftAction::QUARANTINE_VENUE;
            out.severity = SeverityLevel::CRITICAL;
            out.quarantine = true;
            return out;
        }

        out.action = DriftAction::CANCEL_ALL_ORDERS;
        out.severity = SeverityLevel::WARNING;
        return out;
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

        for (size_t i = 0; i < snapshot.fills.size; ++i) {
            const auto& fill = snapshot.fills.items[i];
            if (fill.quantity < 0.0 || fill.price < 0.0 || fill.notional < 0.0 || fill.fee < 0.0) {
                out.mismatch = true;
                out.action = DriftAction::QUARANTINE_VENUE;
                out.reason = "negative fill values";
                return out;
            }
            if (fill.notional > 0.0 && drift(fill.notional, fill.quantity * fill.price) >
                                           thresholds_.max_fill_notional_drift) {
                out.mismatch = true;
                out.action = DriftAction::QUARANTINE_VENUE;
                out.reason = "fill notional mismatch";
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
                return classify_order_drift(MismatchClass::MISSING_ORDER, "missing_order");
            }
            if (drift(venue_order->quantity, internal_order.quantity) >
                    thresholds_.max_order_fill_gap ||
                drift(venue_order->filled_quantity, internal_order.filled_quantity) >
                    thresholds_.max_order_fill_gap) {
                return classify_order_drift(MismatchClass::QTY_DRIFT, "qty_drift");
            }
        }

        for (size_t i = 0; i < venue.open_orders.size; ++i) {
            const ReconciledOrder& venue_order = venue.open_orders.items[i];
            if (!find_order(canonical, venue_order)) {
                return classify_order_drift(MismatchClass::MISSING_ORDER, "missing_order");
            }
        }

        if (fill_gap_detected(venue, canonical)) {
            return classify_fill_gap();
        }

        for (size_t i = 0; i < canonical.balances.size; ++i) {
            const ReconciledBalance& internal_balance = canonical.balances.items[i];
            const ReconciledBalance* venue_balance = find_balance(venue, internal_balance.asset);
            if (!venue_balance ||
                drift(venue_balance->total, internal_balance.total) >
                    thresholds_.max_balance_drift ||
                drift(venue_balance->available, internal_balance.available) >
                    thresholds_.max_balance_drift) {
                return mismatch(MismatchClass::BALANCE_DRIFT,
                                DriftAction::RISK_HALT_RECOMMENDED, SeverityLevel::CRITICAL,
                                "balance_drift", true);
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
                                DriftAction::RISK_HALT_RECOMMENDED, SeverityLevel::CRITICAL,
                                "position_drift", true);
            }
        }

        return out;
    }

    DriftDecision classify_order_drift(MismatchClass mismatch_class, const char* reason) const {
        DriftDecision out;
        out.mismatch = true;
        out.mismatch_class = mismatch_class;
        out.reason = reason;
        out.action = DriftAction::CANCEL_ALL_ORDERS;
        out.severity = SeverityLevel::WARNING;
        return out;
    }

    DriftDecision classify_fill_gap() const {
        DriftDecision out;
        out.mismatch = true;
        out.mismatch_class = MismatchClass::FILL_GAP;
        out.reason = "fill_gap";
        out.action = DriftAction::REQUEST_FILL_REPLAY;
        out.severity = SeverityLevel::WARNING;
        return out;
    }

    static DriftDecision mismatch(MismatchClass mismatch_class, DriftAction action,
                                  SeverityLevel severity, const char* reason, bool quarantine) {
        DriftDecision out;
        out.mismatch = true;
        out.mismatch_class = mismatch_class;
        out.action = action;
        out.severity = severity;
        out.quarantine = quarantine;
        out.reason = reason;
        return out;
    }

    void apply_decision(size_t idx, DriftDecision decision, bool reconnect_phase) {
        auto& state = states_[idx];
        state.last_mismatch = decision.mismatch_class;
        state.last_action = decision.action;
        state.last_severity = decision.severity;

        if (decision.mismatch_class == MismatchClass::FILL_GAP) {
            ++state.fill_gap_retries;
            decision.retry_count = state.fill_gap_retries;
            if (state.fill_gap_retries > policy_.fill_gap_retry_budget) {
                decision.action = DriftAction::RISK_HALT_RECOMMENDED;
                decision.severity = SeverityLevel::CRITICAL;
                decision.quarantine = true;
            } else {
                ++state.fill_replay_requests;
            }
        } else if (decision.mismatch_class == MismatchClass::MISSING_ORDER ||
                   decision.mismatch_class == MismatchClass::QTY_DRIFT) {
            ++state.order_drift_retries;
            decision.retry_count = state.order_drift_retries;
            if (state.order_drift_retries > policy_.order_drift_retry_budget) {
                decision.action = DriftAction::RISK_HALT_RECOMMENDED;
                decision.severity = SeverityLevel::CRITICAL;
                decision.quarantine = true;
            }
        }

        state.last_action = decision.action;
        state.last_severity = decision.severity;
        record_incident(idx, decision, reconnect_phase);
        fire_hook(state.exchange, decision);

        if (decision.quarantine || decision.action == DriftAction::QUARANTINE_VENUE ||
            decision.action == DriftAction::RISK_HALT_RECOMMENDED) {
            state.quarantined = true;
            ++state.mismatch_count;
            LOG_ERROR("reconciliation quarantine", "exchange", exchange_to_string(state.exchange),
                      "reason", decision.reason, "action", static_cast<int>(decision.action));
        }
    }

    void fire_hook(Exchange exchange, const DriftDecision& decision) {
        if (decision.action == DriftAction::CANCEL_ALL_ORDERS && cancel_all_hook_)
            cancel_all_hook_(exchange, decision.mismatch_class, decision.reason);
        if (decision.action == DriftAction::RISK_HALT_RECOMMENDED && risk_halt_hook_)
            risk_halt_hook_(exchange, decision.mismatch_class, decision.reason);
    }

    void record_incident(size_t idx, const DriftDecision& decision, bool reconnect_phase) {
        if (incident_count_ >= incident_trail_.size()) {
            ++dropped_incident_count_;
            return;
        }

        ReconciliationIncident& incident = incident_trail_[incident_count_++];
        incident.ts_ns = now_ns();
        incident.exchange = states_[idx].exchange;
        incident.mismatch_class = decision.mismatch_class;
        incident.action = decision.action;
        incident.severity = decision.severity;
        incident.reconnect_phase = reconnect_phase;
        incident.retry_count = decision.retry_count;
        std::strncpy(incident.reason, decision.reason, sizeof(incident.reason) - 1);
        incident.reason[sizeof(incident.reason) - 1] = '\0';
    }

    void reset_drift_retries(size_t idx) {
        states_[idx].order_drift_retries = 0;
        states_[idx].fill_gap_retries = 0;
    }

    void reset_snapshot_failures(size_t idx) { states_[idx].snapshot_failure_retries = 0; }

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

    static bool is_empty_trade_id(const char* venue_trade_id) noexcept {
        return venue_trade_id[0] == '\0';
    }

    static bool fill_same_identity(const FillIdentity& lhs, const FillIdentity& rhs) noexcept {
        if (lhs.exchange != rhs.exchange)
            return false;

        if (!is_empty_trade_id(lhs.venue_trade_id) && !is_empty_trade_id(rhs.venue_trade_id) &&
            venue_id_eq(lhs.venue_trade_id, rhs.venue_trade_id)) {
            return true;
        }

        if (lhs.client_order_id != 0 && rhs.client_order_id != 0 &&
            lhs.client_order_id == rhs.client_order_id &&
            lhs.exchange_ts_ns == rhs.exchange_ts_ns &&
            drift(lhs.quantity, rhs.quantity) <= 1e-12 && drift(lhs.price, rhs.price) <= 1e-12) {
            return true;
        }

        if (!is_empty_venue_id(lhs.venue_order_id) && !is_empty_venue_id(rhs.venue_order_id) &&
            venue_id_eq(lhs.venue_order_id, rhs.venue_order_id) &&
            lhs.exchange_ts_ns == rhs.exchange_ts_ns &&
            drift(lhs.quantity, rhs.quantity) <= 1e-12 && drift(lhs.price, rhs.price) <= 1e-12) {
            return true;
        }

        return false;
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

    bool build_fill_ledger(const ReconciliationSnapshot& snapshot,
                           FillLedger& ledger) const noexcept {
        std::array<FillIdentity, MAX_FILL_KEYS> dedupe_keys{};
        size_t dedupe_key_count = 0;
        ledger = {};

        for (size_t i = 0; i < snapshot.fills.size; ++i) {
            const ReconciledFill& fill = snapshot.fills.items[i];

            FillIdentity key;
            key.client_order_id = fill.client_order_id;
            std::memcpy(key.venue_order_id, fill.venue_order_id, sizeof(key.venue_order_id));
            std::memcpy(key.venue_trade_id, fill.venue_trade_id, sizeof(key.venue_trade_id));
            key.exchange_ts_ns = fill.exchange_ts_ns;
            key.quantity = fill.quantity;
            key.price = fill.price;
            key.exchange = fill.exchange;

            bool duplicate = false;
            for (size_t k = 0; k < dedupe_key_count; ++k) {
                if (fill_same_identity(key, dedupe_keys[k])) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate)
                continue;

            if (dedupe_key_count >= dedupe_keys.size()) {
                ledger.overflow = true;
                return false;
            }
            dedupe_keys[dedupe_key_count++] = key;

            ledger.cum_qty += fill.quantity;
            const double computed_notional =
                fill.notional > 0.0 ? fill.notional : fill.quantity * fill.price;
            ledger.cum_notional += computed_notional;
            ledger.cum_fee += fill.fee;
            ++ledger.unique_fill_count;
        }

        return true;
    }

    bool fill_gap_detected(const ReconciliationSnapshot& venue,
                           const ReconciliationSnapshot& canonical) const noexcept {
        FillLedger venue_ledger;
        FillLedger canonical_ledger;
        if (!build_fill_ledger(venue, venue_ledger) ||
            !build_fill_ledger(canonical, canonical_ledger))
            return true;

        if (drift(venue_ledger.cum_qty, canonical_ledger.cum_qty) > thresholds_.max_order_fill_gap)
            return true;
        if (drift(venue_ledger.cum_notional, canonical_ledger.cum_notional) >
            thresholds_.max_fill_notional_drift) {
            return true;
        }
        if (drift(venue_ledger.cum_fee, canonical_ledger.cum_fee) > thresholds_.max_fill_fee_drift)
            return true;

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
    RemediationPolicy policy_;
    std::array<LiveConnectorBase*, MAX_CONNECTORS> connectors_{};
    std::array<VenueState, MAX_CONNECTORS> states_{};
    std::array<ReconciliationSnapshot, MAX_CONNECTORS> canonical_snapshots_{};
    std::array<CanonicalSnapshotFetcher, MAX_CONNECTORS> canonical_snapshot_fetchers_{};
    std::array<bool, MAX_CONNECTORS> has_canonical_snapshot_{};
    RemediationHook cancel_all_hook_;
    RemediationHook risk_halt_hook_;
    std::array<ReconciliationIncident, MAX_INCIDENT_TRAIL> incident_trail_{};
    size_t incident_count_ = 0;
    uint32_t dropped_incident_count_ = 0;
    size_t connector_count_ = 0;
};

} // namespace trading
