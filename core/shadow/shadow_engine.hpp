#pragma once

#include "../common/logging.hpp"
#include "../common/types.hpp"
#include "../execution/common/connectors/exchange_connector.hpp"
#include "../execution/common/orders/order_manager.hpp"
#include "../feeds/common/book_manager.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace trading {
    enum class ShadowUrgency : uint8_t {
        PASSIVE = 0,
        BALANCED = 1,
        AGGRESSIVE = 2,
    };

    enum class ShadowExecutionState : uint8_t {
        FLAT = 0,
        ENTERING = 1,
        HOLDING = 2,
        REDUCING = 3,
        FLATTENING = 4,
        HALTED = 5,
    };

    struct ShadowIntentMetadata {
        char intent[24] = "unspecified";
        char reason[48] = "none";
        double signal_bps = 0.0;
        double risk_score = 0.0;
        double target_position = 0.0;
        double current_position = 0.0;
        double expected_cost_bps = 0.0;
        double expected_edge_bps = 0.0;
        double max_shortfall_bps = 0.0;
        int64_t decision_ts_ns = 0;
        ShadowUrgency urgency = ShadowUrgency::BALANCED;
    };

    struct ShadowConfig {
        double binance_maker_fee_bps = 2.0;
        double binance_taker_fee_bps = 5.0;
        double kraken_maker_fee_bps = 2.0;
        double kraken_taker_fee_bps = 5.0;
        double okx_maker_fee_bps = 2.0;
        double okx_taker_fee_bps = 5.0;
        double coinbase_maker_fee_bps = 4.0;
        double coinbase_taker_fee_bps = 8.0;
        int64_t base_latency_ns = 150000;
        int64_t latency_jitter_ns = 50000;
        double impact_slippage_per_notional_bps = 0.8;
        double queue_match_fraction_per_check = 0.35;
        char log_path[256] = "shadow_decisions.jsonl";

        static ShadowConfig from_yaml_values(double bin_maker_bps, double bin_taker_bps,
                                             double kra_maker_bps, double kra_taker_bps,
                                             double okx_maker_bps = 2.0, double okx_taker_bps = 5.0,
                                             double coinbase_maker_bps = 4.0,
                                             double coinbase_taker_bps = 8.0,
                                             const char *log_file = "shadow_decisions.jsonl") noexcept {
            ShadowConfig c;
            c.binance_maker_fee_bps = bin_maker_bps;
            c.binance_taker_fee_bps = bin_taker_bps;
            c.kraken_maker_fee_bps = kra_maker_bps;
            c.kraken_taker_fee_bps = kra_taker_bps;
            c.okx_maker_fee_bps = okx_maker_bps;
            c.okx_taker_fee_bps = okx_taker_bps;
            c.coinbase_maker_fee_bps = coinbase_maker_bps;
            c.coinbase_taker_fee_bps = coinbase_taker_bps;
            std::strncpy(c.log_path, log_file, sizeof(c.log_path) - 1);
            c.log_path[sizeof(c.log_path) - 1] = '\0';
            return c;
        }
    };

    struct ShadowStateTransition {
        ShadowExecutionState previous = ShadowExecutionState::FLAT;
        ShadowExecutionState current = ShadowExecutionState::FLAT;
        char reason[48] = "init";
        double current_position = 0.0;
        double target_position = 0.0;
        bool changed = false;
    };

    class ShadowStateMachine {
    public:
        ShadowStateTransition evaluate(double current_position, double target_position,
                                       bool flatten_now, bool halted,
                                       const char *reason) noexcept {
            ShadowStateTransition out;
            out.previous = state_;
            out.current = next_state(current_position, target_position, flatten_now, halted);
            out.current_position = current_position;
            out.target_position = target_position;
            copy_reason(out.reason, reason);
            out.changed = out.previous != out.current;
            state_ = out.current;
            return out;
        }

        [[nodiscard]] ShadowExecutionState state() const noexcept { return state_; }

        static const char *to_string(ShadowExecutionState state) noexcept {
            switch (state) {
                case ShadowExecutionState::FLAT:
                    return "FLAT";
                case ShadowExecutionState::ENTERING:
                    return "ENTERING";
                case ShadowExecutionState::HOLDING:
                    return "HOLDING";
                case ShadowExecutionState::REDUCING:
                    return "REDUCING";
                case ShadowExecutionState::FLATTENING:
                    return "FLATTENING";
                case ShadowExecutionState::HALTED:
                    return "HALTED";
            }
            return "FLAT";
        }

    private:
        static void copy_reason(char (&dst)[48], const char *src) noexcept {
            if (!src) {
                dst[0] = '\0';
                return;
            }
            std::strncpy(dst, src, sizeof(dst) - 1);
            dst[sizeof(dst) - 1] = '\0';
        }

        static ShadowExecutionState next_state(double current_position, double target_position,
                                               bool flatten_now, bool halted) noexcept {
            constexpr double k_pos_epsilon = 1e-6;
            if (halted)
                return ShadowExecutionState::HALTED;
            if (flatten_now)
                return std::abs(current_position) <= k_pos_epsilon
                               ? ShadowExecutionState::FLAT
                               : ShadowExecutionState::FLATTENING;
            if (std::abs(current_position) <= k_pos_epsilon &&
                std::abs(target_position) <= k_pos_epsilon) {
                return ShadowExecutionState::FLAT;
            }
            const double current_abs = std::abs(current_position);
            const double target_abs = std::abs(target_position);
            const bool same_direction =
                    current_abs <= k_pos_epsilon || target_abs <= k_pos_epsilon ||
                    ((current_position > 0.0) == (target_position > 0.0));
            if (current_abs <= k_pos_epsilon && target_abs > k_pos_epsilon)
                return ShadowExecutionState::ENTERING;
            if (!same_direction)
                return ShadowExecutionState::FLATTENING;
            const double delta = target_abs - current_abs;
            if (delta > k_pos_epsilon)
                return ShadowExecutionState::ENTERING;
            if (delta < -k_pos_epsilon)
                return target_abs <= k_pos_epsilon ? ShadowExecutionState::FLATTENING
                                                   : ShadowExecutionState::REDUCING;
            return current_abs > k_pos_epsilon ? ShadowExecutionState::HOLDING
                                               : ShadowExecutionState::FLAT;
        }

        ShadowExecutionState state_ = ShadowExecutionState::FLAT;
    };

    struct ShadowOrder {
        uint64_t client_order_id = 0;
        char symbol[16] = {};
        Exchange exchange = Exchange::UNKNOWN;
        Side side;
        OrderType type;
        TimeInForce tif;
        double price = 0;
        double quantity = 0;
        double filled_qty = 0;
        double notional_filled = 0;
        double queue_ahead_qty = 0;
        int64_t submit_ts_ns = 0;
        int64_t release_ts_ns = 0;
        bool active = false;
        bool is_maker = false;
        ShadowIntentMetadata intent{};
        double decision_price = 0;
        double decision_mid = 0;
    };

    class ShadowConnector : public ExchangeConnector {
    public:
        static constexpr size_t MAX_SHADOW_ORDERS = 256;

        explicit ShadowConnector(Exchange ex, const ShadowConfig &cfg, BookManager &book)
            : ex_(ex), cfg_(cfg), book_(book) {
            log_fp_ = std::fopen(cfg.log_path, "a");
        }

        ~ShadowConnector() {
            if (log_fp_)
                std::fclose(log_fp_);
        }

        Exchange exchange_id() const override { return ex_; }
        bool is_connected() const override { return true; }

        ConnectorResult connect() override { return ConnectorResult::OK; }

        void disconnect() override {
        }

        ConnectorResult submit_order(const Order &order) override {
            ShadowOrder *slot = nullptr;
            for (auto &s: orders_) {
                if (!s.active) {
                    slot = &s;
                    break;
                }
            }
            if (!slot) {
                LOG_ERROR("Shadow order pool full", "component", "shadow_connector");
                return ConnectorResult::ERROR_UNKNOWN;
            }

            *slot = {};
            slot->client_order_id = order.client_order_id;
            copy_symbol(slot->symbol, order.symbol);
            slot->exchange = order.exchange;
            slot->side = order.side;
            slot->type = order.type;
            slot->tif = order.tif;
            slot->price = order.price;
            slot->quantity = order.quantity;
            slot->submit_ts_ns = now_ns();
            slot->release_ts_ns = slot->submit_ts_ns + modeled_latency_ns(order.client_order_id);
            slot->active = true;
            slot->is_maker = false;
            slot->queue_ahead_qty = queue_ahead_at_submit(*slot);
            slot->intent = next_intent_;
            if (slot->intent.decision_ts_ns <= 0)
                slot->intent.decision_ts_ns = slot->submit_ts_ns;
            slot->decision_price = decision_price(order.side);
            slot->decision_mid = book_.mid_price();

            const bool fillable_now = is_immediately_fillable(*slot);
            const bool released = slot->submit_ts_ns >= slot->release_ts_ns;

            if (released && (order.type == OrderType::MARKET || fillable_now)) {
                fill_at_best(*slot, slot->quantity);
                return ConnectorResult::OK;
            }

            slot->is_maker = true;
            log_order(*slot, "RESTING");
            return ConnectorResult::OK;
        }

        ConnectorResult cancel_order(uint64_t client_id) override {
            for (auto &s: orders_) {
                if (s.active && s.client_order_id == client_id) {
                    log_order(s, "CANCELED");
                    emit_cancel(s);
                    s.active = false;
                    return ConnectorResult::OK;
                }
            }
            return ConnectorResult::ERROR_UNKNOWN;
        }

        ConnectorResult replace_order(uint64_t client_order_id, const Order &replacement) override {
            ConnectorResult cancel_res = cancel_order(client_order_id);
            if (cancel_res != ConnectorResult::OK)
                return cancel_res;
            return submit_order(replacement);
        }

        ConnectorResult query_order(uint64_t client_order_id, FillUpdate &status) override {
            for (const auto &s: orders_) {
                if (s.active && s.client_order_id == client_order_id) {
                    status.client_order_id = client_order_id;
                    status.fill_qty = s.filled_qty;
                    status.cumulative_filled_qty = s.filled_qty;
                    status.avg_fill_price =
                            s.filled_qty > 0.0 ? (s.notional_filled / s.filled_qty) : 0.0;
                    status.new_state =
                            s.filled_qty > 0.0 ? OrderState::PARTIALLY_FILLED : OrderState::OPEN;
                    status.local_ts_ns = now_ns();
                    return ConnectorResult::OK;
                }
            }
            return ConnectorResult::ERROR_INVALID_ORDER;
        }

        ConnectorResult cancel_all(const char *symbol) override {
            for (auto &s: orders_) {
                if (s.active && std::strncmp(s.symbol, symbol, 15) == 0) {
                    emit_cancel(s);
                    s.active = false;
                }
            }
            return ConnectorResult::OK;
        }

        ConnectorResult reconcile() override { return ConnectorResult::OK; }

        void check_fills() {
            const int64_t ts_now = now_ns();
            for (auto &s: orders_) {
                if (!s.active || ts_now < s.release_ts_ns)
                    continue;
                if (s.type == OrderType::MARKET) {
                    fill_at_best(s, s.quantity - s.filled_qty);
                    continue;
                }
                const bool fillable = is_immediately_fillable(s);
                if (s.tif == TimeInForce::IOC && !fillable) {
                    emit_cancel(s);
                    s.active = false;
                    continue;
                }
                if (fillable)
                    fill_passive_or_cross(s);
            }
        }

        double total_pnl() const noexcept { return total_pnl_.load(std::memory_order_acquire); }
        uint64_t total_fills() const noexcept { return total_fills_.load(std::memory_order_acquire); }

        uint64_t opened_positions() const noexcept {
            return opened_positions_.load(std::memory_order_acquire);
        }

        double net_position() const noexcept { return net_position_.load(std::memory_order_acquire); }

        uint32_t active_orders() const noexcept {
            uint32_t n = 0;
            for (const auto &s: orders_)
                if (s.active)
                    ++n;
            return n;
        }

        void set_intent_metadata(const ShadowIntentMetadata &intent) noexcept { next_intent_ = intent; }

    private:
        static void copy_symbol(char (&dst)[16], const char (&src)[16]) noexcept {
            std::memcpy(dst, src, sizeof(dst));
            dst[sizeof(dst) - 1] = '\0';
        }

        Exchange ex_;
        ShadowConfig cfg_;
        BookManager &book_;
        std::FILE *log_fp_ = nullptr;
        std::array<ShadowOrder, MAX_SHADOW_ORDERS> orders_{};
        ShadowIntentMetadata next_intent_{};

        std::atomic<double> total_pnl_{0.0};
        std::atomic<uint64_t> total_fills_{0};
        std::atomic<uint64_t> opened_positions_{0};
        std::atomic<double> net_position_{0.0};

        bool is_immediately_fillable(const ShadowOrder &s) const noexcept {
            if (s.type == OrderType::MARKET)
                return true;
            if (s.side == Side::BID) {
                double best_ask = book_.best_ask();
                return (best_ask > 0.0 && s.price >= best_ask);
            }
            double best_bid = book_.best_bid();
            return (best_bid > 0.0 && s.price <= best_bid);
        }

        void fill_passive_or_cross(ShadowOrder &s) {
            const double remaining = s.quantity - s.filled_qty;
            if (remaining <= 0.0) {
                s.active = false;
                return;
            }

            const double top_size = top_opposite_size(s.side);
            if (top_size <= 0.0) {
                emit_reject(s, "no_liquidity");
                s.active = false;
                return;
            }

            double available = top_size * cfg_.queue_match_fraction_per_check;
            if (s.queue_ahead_qty > 0.0) {
                const double consumed = available > s.queue_ahead_qty ? s.queue_ahead_qty : available;
                s.queue_ahead_qty -= consumed;
                available -= consumed;
            }
            if (available <= 0.0)
                return;

            const double fill_qty = available > remaining ? remaining : available;
            fill_at_best(s, fill_qty);
        }

        void fill_at_best(ShadowOrder &s, double qty) {
            if (qty <= 0.0)
                return;

            double fill_px = (s.side == Side::BID) ? book_.best_ask() : book_.best_bid();
            if (fill_px <= 0.0) {
                emit_reject(s, "no_liquidity");
                s.active = false;
                return;
            }

            fill_px = apply_slippage(fill_px, qty, s.side);
            apply_fill(s, fill_px, qty);
        }

        void apply_fill(ShadowOrder &s, double fill_px, double fill_qty) {
            if (fill_qty <= 0.0)
                return;

            const bool opened_position = s.filled_qty <= 1e-12;
            s.filled_qty += fill_qty;
            s.notional_filled += fill_px * fill_qty;
            if (s.filled_qty >= s.quantity - 1e-12) {
                s.filled_qty = s.quantity;
                s.active = false;
            }

            const double fee_bps = fee_for(s);
            const double fee = (fee_bps / 10000.0) * fill_px * fill_qty;
            const double sign = (s.side == Side::ASK) ? 1.0 : -1.0;
            const double contrib = sign * fill_px * fill_qty - sign * fee;

            const double old_pnl = total_pnl_.load(std::memory_order_acquire);
            total_pnl_.store(old_pnl + contrib, std::memory_order_release);
            total_fills_.fetch_add(1, std::memory_order_relaxed);
            const double position_delta = (s.side == Side::BID) ? fill_qty : -fill_qty;
            const double old_position = net_position_.load(std::memory_order_acquire);
            net_position_.store(old_position + position_delta, std::memory_order_release);
            if (opened_position) {
                opened_positions_.fetch_add(1, std::memory_order_relaxed);
            }

            log_fill(s, fill_px, fill_qty, fee_bps, fee);
            emit_fill(s, fill_px, fill_qty);
        }

        double fee_for(const ShadowOrder &s) const noexcept {
            const bool maker = s.is_maker;
            switch (s.exchange) {
                case Exchange::BINANCE:
                    return maker ? cfg_.binance_maker_fee_bps : cfg_.binance_taker_fee_bps;
                case Exchange::KRAKEN:
                    return maker ? cfg_.kraken_maker_fee_bps : cfg_.kraken_taker_fee_bps;
                case Exchange::OKX:
                    return maker ? cfg_.okx_maker_fee_bps : cfg_.okx_taker_fee_bps;
                case Exchange::COINBASE:
                    return maker ? cfg_.coinbase_maker_fee_bps : cfg_.coinbase_taker_fee_bps;
                default:
                    return maker ? cfg_.kraken_maker_fee_bps : cfg_.kraken_taker_fee_bps;
            }
        }

        void emit_fill(const ShadowOrder &s, double fill_px, double fill_qty) {
            FillUpdate u;
            u.client_order_id = s.client_order_id;
            u.fill_price = fill_px;
            u.fill_qty = fill_qty;
            u.cumulative_filled_qty = s.filled_qty;
            u.avg_fill_price = s.filled_qty > 0.0 ? (s.notional_filled / s.filled_qty) : 0.0;
            u.new_state =
                    s.filled_qty >= s.quantity ? OrderState::FILLED : OrderState::PARTIALLY_FILLED;
            u.local_ts_ns = now_ns();
            ExchangeConnector::emit_fill(u);
        }

        void emit_cancel(const ShadowOrder &s) {
            FillUpdate u;
            u.client_order_id = s.client_order_id;
            u.new_state = OrderState::CANCELED;
            u.local_ts_ns = now_ns();
            ExchangeConnector::emit_fill(u);
        }

        void emit_reject(const ShadowOrder &s, const char *reason) {
            FillUpdate u;
            u.client_order_id = s.client_order_id;
            u.new_state = OrderState::REJECTED;
            u.local_ts_ns = now_ns();
            std::strncpy(u.reject_reason, reason, 63);
            ExchangeConnector::emit_fill(u);
        }

        void log_order(const ShadowOrder &s, const char *event) {
            if (!log_fp_)
                return;
            const double best_bid = book_.best_bid();
            const double best_ask = book_.best_ask();
            std::fprintf(log_fp_,
                         "{\"event\":\"%s\",\"ts_ns\":%lld,\"exchange\":\"%s\","
                         "\"symbol\":\"%s\",\"side\":\"%s\",\"type\":\"%s\","
                         "\"price\":%.2f,\"qty\":%.6f,"
                         "\"book_bid\":%.2f,\"book_ask\":%.2f,"
                         "\"client_oid\":%llu,\"intent\":\"%s\",\"reason\":\"%s\","
                         "\"signal_bps\":%.4f,\"risk_score\":%.4f,\"target_position\":%.6f,"
                         "\"current_position\":%.6f,\"expected_cost_bps\":%.4f,"
                         "\"expected_edge_bps\":%.4f,\"max_shortfall_bps\":%.4f,"
                         "\"decision_px\":%.6f,\"decision_mid\":%.6f,\"urgency\":\"%s\"}\n",
                         event, (long long) now_ns(), exchange_to_string(s.exchange), s.symbol,
                         s.side == Side::BID ? "BID" : "ASK",
                         s.type == OrderType::LIMIT ? "LIMIT" : "MARKET", s.price, s.quantity, best_bid,
                         best_ask, (unsigned long long) s.client_order_id, s.intent.intent,
                         s.intent.reason, s.intent.signal_bps, s.intent.risk_score,
                         s.intent.target_position, s.intent.current_position,
                         s.intent.expected_cost_bps, s.intent.expected_edge_bps,
                         s.intent.max_shortfall_bps, s.decision_price, s.decision_mid,
                         urgency_to_string(s.intent.urgency));
            std::fflush(log_fp_);
        }

        void log_fill(const ShadowOrder &s, double fill_px, double fill_qty, double fee_bps,
                      double fee) {
            if (!log_fp_)
                return;
            std::fprintf(log_fp_,
                         "{\"event\":\"FILL\",\"ts_ns\":%lld,\"exchange\":\"%s\","
                         "\"symbol\":\"%s\",\"side\":\"%s\",\"maker\":%s,"
                         "\"fill_px\":%.2f,\"qty\":%.6f,\"cum_qty\":%.6f,"
                         "\"fee_bps\":%.2f,\"fee_usd\":%.6f,"
                         "\"cumulative_pnl\":%.6f,\"client_oid\":%llu,"
                         "\"intent\":\"%s\",\"reason\":\"%s\",\"signal_bps\":%.4f,"
                         "\"risk_score\":%.4f,\"target_position\":%.6f,"
                         "\"current_position\":%.6f,\"expected_cost_bps\":%.4f,"
                         "\"expected_edge_bps\":%.4f,\"max_shortfall_bps\":%.4f,"
                         "\"decision_px\":%.6f,\"decision_mid\":%.6f,\"book_mid\":%.6f,"
                         "\"implementation_shortfall_bps\":%.4f,\"edge_at_entry_bps\":%.4f,"
                         "\"hold_time_ms\":%.3f,\"markout_bps\":%.4f,\"urgency\":\"%s\"}\n",
                         (long long) now_ns(), exchange_to_string(s.exchange), s.symbol,
                         s.side == Side::BID ? "BID" : "ASK", s.is_maker ? "true" : "false", fill_px,
                         fill_qty, s.filled_qty, fee_bps, fee,
                         total_pnl_.load(std::memory_order_relaxed),
                         (unsigned long long) s.client_order_id, s.intent.intent, s.intent.reason,
                         s.intent.signal_bps, s.intent.risk_score, s.intent.target_position,
                         s.intent.current_position, s.intent.expected_cost_bps,
                         s.intent.expected_edge_bps, s.intent.max_shortfall_bps, s.decision_price,
                         s.decision_mid, book_.mid_price(),
                         implementation_shortfall_bps(s, fill_px), edge_at_entry_bps(s, fill_px),
                         static_cast<double>(now_ns() - s.intent.decision_ts_ns) / 1000000.0,
                         markout_bps(s, fill_px), urgency_to_string(s.intent.urgency));
            std::fflush(log_fp_);
        }

        int64_t modeled_latency_ns(uint64_t client_order_id) const noexcept {
            if (cfg_.latency_jitter_ns <= 0)
                return cfg_.base_latency_ns;
            const uint64_t spread = static_cast<uint64_t>(cfg_.latency_jitter_ns) * 2ULL + 1ULL;
            const int64_t jitter =
                    static_cast<int64_t>(client_order_id % spread) - cfg_.latency_jitter_ns;
            return cfg_.base_latency_ns + jitter;
        }

        double queue_ahead_at_submit(const ShadowOrder &s) const noexcept {
            std::vector<PriceLevel> bids;
            std::vector<PriceLevel> asks;
            book_.get_top_levels(8, bids, asks);
            const auto &side_levels = s.side == Side::BID ? bids : asks;
            for (const auto &lvl: side_levels)
                if (lvl.price == s.price)
                    return lvl.size * 0.6;
            return 0.0;
        }

        double top_opposite_size(Side side) const noexcept {
            std::vector<PriceLevel> bids;
            std::vector<PriceLevel> asks;
            book_.get_top_levels(1, bids, asks);
            if (side == Side::BID)
                return asks.empty() ? 0.0 : asks.front().size;
            return bids.empty() ? 0.0 : bids.front().size;
        }

        double apply_slippage(double px, double qty, Side side) const noexcept {
            std::vector<PriceLevel> bids;
            std::vector<PriceLevel> asks;
            book_.get_top_levels(1, bids, asks);
            const double top_size = side == Side::BID
                                        ? (asks.empty() ? 0.0 : asks.front().size)
                                        : (bids.empty() ? 0.0 : bids.front().size);
            if (top_size <= 0.0)
                return px;
            const double impact_bps = cfg_.impact_slippage_per_notional_bps * (qty / top_size);
            const double sign = side == Side::BID ? 1.0 : -1.0;
            return px * (1.0 + sign * (impact_bps / 10000.0));
        }

        double decision_price(Side side) const noexcept {
            return side == Side::BID ? book_.best_ask() : book_.best_bid();
        }

        static const char *urgency_to_string(ShadowUrgency urgency) noexcept {
            switch (urgency) {
                case ShadowUrgency::PASSIVE:
                    return "PASSIVE";
                case ShadowUrgency::BALANCED:
                    return "BALANCED";
                case ShadowUrgency::AGGRESSIVE:
                    return "AGGRESSIVE";
                default:
                    return "BALANCED";
            }
        }

        double implementation_shortfall_bps(const ShadowOrder &s, double fill_px) const noexcept {
            if (s.decision_price <= 0.0)
                return 0.0;
            const double raw_bps = (fill_px - s.decision_price) / s.decision_price * 10000.0;
            return s.side == Side::BID ? raw_bps : -raw_bps;
        }

        double edge_at_entry_bps(const ShadowOrder &s, double fill_px) const noexcept {
            const double alpha_edge = s.side == Side::BID ? s.intent.signal_bps : -s.intent.signal_bps;
            return alpha_edge - implementation_shortfall_bps(s, fill_px);
        }

        double markout_bps(const ShadowOrder &s, double fill_px) const noexcept {
            const double mark = book_.mid_price();
            if (mark <= 0.0 || fill_px <= 0.0)
                return 0.0;
            const double raw_bps = (mark - fill_px) / fill_px * 10000.0;
            return s.side == Side::BID ? raw_bps : -raw_bps;
        }

        static int64_t now_ns() noexcept {
            using namespace std::chrono;
            return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
        }
    };

    class ShadowEngine {
    public:
        ShadowEngine(BookManager &binance_book, BookManager &kraken_book,
                     BookManager &okx_book, BookManager &coinbase_book,
                     const ShadowConfig &cfg = {})
            : cfg_(cfg),
              binance_shadow_(Exchange::BINANCE, cfg, binance_book),
              kraken_shadow_(Exchange::KRAKEN, cfg, kraken_book),
              okx_shadow_(Exchange::OKX, cfg, okx_book),
              coinbase_shadow_(Exchange::COINBASE, cfg, coinbase_book) {
            log_fp_ = std::fopen(cfg.log_path, "a");
        }

        ~ShadowEngine() {
            if (log_fp_)
                std::fclose(log_fp_);
        }

        ShadowConnector &binance_connector() { return binance_shadow_; }
        ShadowConnector &kraken_connector() { return kraken_shadow_; }
        ShadowConnector &okx_connector() { return okx_shadow_; }
        ShadowConnector &coinbase_connector() { return coinbase_shadow_; }

        void check_fills() {
            binance_shadow_.check_fills();
            kraken_shadow_.check_fills();
            okx_shadow_.check_fills();
            coinbase_shadow_.check_fills();
        }

        double net_pnl() const noexcept {
            return binance_shadow_.total_pnl() + kraken_shadow_.total_pnl()
                   + okx_shadow_.total_pnl() + coinbase_shadow_.total_pnl();
        }

        uint64_t total_fills() const noexcept {
            return binance_shadow_.total_fills() + kraken_shadow_.total_fills()
                   + okx_shadow_.total_fills() + coinbase_shadow_.total_fills();
        }

        uint64_t opened_positions() const noexcept {
            return binance_shadow_.opened_positions() + kraken_shadow_.opened_positions()
                   + okx_shadow_.opened_positions() + coinbase_shadow_.opened_positions();
        }

        double net_position() const noexcept {
            return binance_shadow_.net_position() + kraken_shadow_.net_position()
                   + okx_shadow_.net_position() + coinbase_shadow_.net_position();
        }

        ShadowStateTransition update_state(double target_position, bool flatten_now,
                                           bool halted, const char *reason) noexcept {
            ShadowStateTransition transition =
                    state_machine_.evaluate(net_position(), target_position, flatten_now, halted, reason);
            if (transition.changed)
                log_transition(transition);
            return transition;
        }

        [[nodiscard]] ShadowExecutionState state() const noexcept { return state_machine_.state(); }

        void log_summary() const {
            LOG_INFO("Shadow session summary",
                     "session_pnl_usd", net_pnl(),
                     "net_pnl_usd", net_pnl(),
                     "net_position", net_position(),
                     "total_fills", total_fills(),
                     "opened_positions", opened_positions(),
                     "state", ShadowStateMachine::to_string(state_machine_.state()),
                     "bin_opened", binance_shadow_.opened_positions(),
                     "kra_opened", kraken_shadow_.opened_positions(),
                     "okx_opened", okx_shadow_.opened_positions(),
                     "cb_opened", coinbase_shadow_.opened_positions(),
                     "bin_active", binance_shadow_.active_orders(),
                     "kra_active", kraken_shadow_.active_orders(),
                     "okx_active", okx_shadow_.active_orders(),
                     "cb_active", coinbase_shadow_.active_orders());
        }

    private:
        void log_transition(const ShadowStateTransition &transition) noexcept {
            if (!log_fp_)
                return;
            using namespace std::chrono;
            const auto ts_ns =
                    duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
            std::fprintf(log_fp_,
                         "{\"event\":\"STATE_TRANSITION\",\"ts_ns\":%lld,\"from\":\"%s\","
                         "\"to\":\"%s\",\"reason\":\"%s\",\"current_position\":%.6f,"
                         "\"target_position\":%.6f}\n",
                         (long long) ts_ns,
                         ShadowStateMachine::to_string(transition.previous),
                         ShadowStateMachine::to_string(transition.current), transition.reason,
                         transition.current_position, transition.target_position);
            std::fflush(log_fp_);
        }

        ShadowConfig cfg_{};
        std::FILE *log_fp_ = nullptr;
        ShadowStateMachine state_machine_{};
        ShadowConnector binance_shadow_;
        ShadowConnector kraken_shadow_;
        ShadowConnector okx_shadow_;
        ShadowConnector coinbase_shadow_;
    };
} // namespace trading
