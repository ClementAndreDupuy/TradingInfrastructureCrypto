#pragma once

// Arbitrage Risk Manager
//
// Pre-trade risk enforcement for cross-exchange arbitrage.
// Hot path target: < 500 nanoseconds per evaluate_arb() call.
//
// Thread safety:
//   - evaluate_arb()     : can be called from the strategy thread
//   - open_leg()         : called from strategy thread after APPROVED verdict
//   - on_buy/sell_filled : MUST be called from a single fill-processing thread
//   - on_leg_rejected    : MUST be called from the fill-processing thread
//   - heartbeat()        : called from the main hot-path loop
//
// No heap allocations after construction. All memory pre-allocated.

#include "../common/types.hpp"
#include "../common/logging.hpp"
#include "kill_switch.hpp"
#include <atomic>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace trading {

// ── Constants ────────────────────────────────────────────────────────────────

static constexpr int  ARB_MAX_SYMBOLS    = 32;
static constexpr int  ARB_MAX_OPEN_LEGS  = 64;
static constexpr int  ARB_NUM_EXCHANGES  = 4;  // must match Exchange enum size

// ── Risk Configuration ───────────────────────────────────────────────────────

struct ArbRiskConfig {
    // Position limits
    double max_abs_position_per_symbol  = 1.0;        // e.g. 1 BTC per exchange
    double max_cross_exchange_exposure  = 2.0;        // Sum of abs positions per symbol

    // Notional limits (USD)
    double max_notional_per_symbol      = 100'000.0;
    double max_notional_per_exchange    = 250'000.0;
    double max_portfolio_notional       = 500'000.0;

    // Drawdown
    double max_drawdown_usd             = -5'000.0;   // Kill if PnL drops below this

    // Rate limits
    uint32_t max_orders_per_second      = 10;
    uint32_t max_orders_per_minute      = 200;

    // Arbitrage-specific
    double min_spread_bps               = 5.0;        // Gross spread must exceed this
    double min_profit_usd               = 0.50;       // After-fee min profit per leg
    int64_t max_book_age_ns             = 500'000'000LL; // 500ms - reject stale books
    uint32_t max_open_arb_legs          = 10;         // Concurrent open legs

    // Flash crash / book corruption guard
    double max_price_deviation_bps      = 300.0;      // 3% from reference rejects

    // Circuit breaker - pause after N consecutive per-leg losses
    uint32_t circuit_breaker_count      = 5;
    double circuit_breaker_loss_usd     = -50.0;      // Per-leg loss threshold

    // Taker fees per exchange in bps (BINANCE=0, OKX=1, COINBASE=2, KRAKEN=3)
    double taker_fee_bps[ARB_NUM_EXCHANGES] = {7.5, 8.0, 6.0, 10.0};
};

// ── Risk Verdict ─────────────────────────────────────────────────────────────

enum class RiskVerdict : uint8_t {
    APPROVED                    = 0,
    REJECTED_KILL_SWITCH        = 1,
    REJECTED_CIRCUIT_BREAKER    = 2,
    REJECTED_DRAWDOWN           = 3,
    REJECTED_STALE_BOOK         = 4,
    REJECTED_SPREAD_TOO_SMALL   = 5,
    REJECTED_FEE_ADJUSTED_LOSS  = 6,
    REJECTED_FLASH_CRASH        = 7,
    REJECTED_MAX_ARB_LEGS       = 8,
    REJECTED_RATE_LIMIT         = 9,
    REJECTED_POSITION_LIMIT     = 10,
    REJECTED_NOTIONAL_PER_SYM   = 11,
    REJECTED_NOTIONAL_PER_VENUE = 12,
    REJECTED_PORTFOLIO_NOTIONAL = 13,
    REJECTED_INVALID_INPUT      = 14,
};

inline const char* verdict_to_string(RiskVerdict v) noexcept {
    switch (v) {
        case RiskVerdict::APPROVED:                    return "APPROVED";
        case RiskVerdict::REJECTED_KILL_SWITCH:        return "KILL_SWITCH";
        case RiskVerdict::REJECTED_CIRCUIT_BREAKER:    return "CIRCUIT_BREAKER";
        case RiskVerdict::REJECTED_DRAWDOWN:           return "DRAWDOWN";
        case RiskVerdict::REJECTED_STALE_BOOK:         return "STALE_BOOK";
        case RiskVerdict::REJECTED_SPREAD_TOO_SMALL:   return "SPREAD_TOO_SMALL";
        case RiskVerdict::REJECTED_FEE_ADJUSTED_LOSS:  return "FEE_ADJUSTED_LOSS";
        case RiskVerdict::REJECTED_FLASH_CRASH:        return "FLASH_CRASH";
        case RiskVerdict::REJECTED_MAX_ARB_LEGS:       return "MAX_ARB_LEGS";
        case RiskVerdict::REJECTED_RATE_LIMIT:         return "RATE_LIMIT";
        case RiskVerdict::REJECTED_POSITION_LIMIT:     return "POSITION_LIMIT";
        case RiskVerdict::REJECTED_NOTIONAL_PER_SYM:   return "NOTIONAL_PER_SYM";
        case RiskVerdict::REJECTED_NOTIONAL_PER_VENUE: return "NOTIONAL_PER_VENUE";
        case RiskVerdict::REJECTED_PORTFOLIO_NOTIONAL: return "PORTFOLIO_NOTIONAL";
        case RiskVerdict::REJECTED_INVALID_INPUT:      return "INVALID_INPUT";
        default:                                       return "UNKNOWN";
    }
}

// ── Arb Opportunity (input to risk check) ───────────────────────────────────

// Fixed-size, no heap allocation. Caller fills this from live book data.
struct ArbOpportunity {
    char     symbol[16];          // Normalized base symbol, e.g. "BTC"
    Exchange buy_exchange;        // Exchange where we BUY (take the ask)
    Exchange sell_exchange;       // Exchange where we SELL (take the bid)
    double   buy_price;           // Best ask on buy exchange (our execution price)
    double   sell_price;          // Best bid on sell exchange (our execution price)
    double   quantity;            // Desired trade size in base units
    double   reference_price;     // Fair-value estimate (e.g. CMC mid) for flash-crash guard
    int64_t  buy_book_ts_ns;      // Timestamp of most recent buy-side book update
    int64_t  sell_book_ts_ns;     // Timestamp of most recent sell-side book update
    int64_t  local_ts_ns;         // Current local timestamp (nanoseconds)

    ArbOpportunity() noexcept {
        std::memset(this, 0, sizeof(ArbOpportunity));
        buy_exchange  = Exchange::UNKNOWN;
        sell_exchange = Exchange::UNKNOWN;
    }
};

// ── Internal Leg State ───────────────────────────────────────────────────────

struct ArbLegState {
    uint64_t  id;
    char      symbol[16];
    Exchange  buy_exchange;
    Exchange  sell_exchange;
    double    quantity;
    double    expected_buy_price;
    double    expected_sell_price;
    double    actual_buy_price;    // Set when buy leg fills
    double    actual_sell_price;   // Set when sell leg fills
    int64_t   open_ts_ns;
    bool      buy_filled;
    bool      sell_filled;
    bool      active;
};

// ── ArbRiskManager ──────────────────────────────────────────────────────────

class ArbRiskManager {
public:
    explicit ArbRiskManager(const ArbRiskConfig& cfg)
        : cfg_(cfg),
          portfolio_pnl_(0.0),
          portfolio_notional_(0.0),
          open_leg_count_(0),
          consecutive_losses_(0),
          circuit_breaker_active_(false),
          rate_second_count_(0),
          rate_minute_count_(0),
          rate_second_start_ns_(0),
          rate_minute_start_ns_(0),
          next_leg_id_(1) {

        static_assert(sizeof(Exchange) == 1, "Exchange enum must be uint8_t");

        // Zero-initialize per-exchange and per-symbol state
        for (int e = 0; e < ARB_NUM_EXCHANGES; ++e) {
            exchange_notional_[e].store(0.0, std::memory_order_relaxed);
            for (int s = 0; s < ARB_MAX_SYMBOLS; ++s) {
                position_[e][s].store(0.0, std::memory_order_relaxed);
            }
        }
        for (auto& leg : legs_) leg.active = false;
        std::memset(symbols_, 0, sizeof(symbols_));
        symbol_count_ = 0;
    }

    // ── Hot Path: evaluate an arb opportunity ────────────────────────────────
    //
    // Checks all risk limits and updates rate counters if APPROVED.
    // Returns APPROVED or the first violated limit.
    // Target latency: < 500 nanoseconds on x86-64.
    RiskVerdict evaluate_arb(const ArbOpportunity& opp) noexcept {
        // ── 1. Kill switch (single atomic load, ~3ns) ──────────────────────
        if (kill_switch_.is_active())
            return RiskVerdict::REJECTED_KILL_SWITCH;

        // ── 2. Circuit breaker ────────────────────────────────────────────
        if (circuit_breaker_active_.load(std::memory_order_acquire))
            return RiskVerdict::REJECTED_CIRCUIT_BREAKER;

        // ── 3. Drawdown ───────────────────────────────────────────────────
        if (portfolio_pnl_.load(std::memory_order_acquire) < cfg_.max_drawdown_usd) {
            kill_switch_.trigger(KillReason::DRAWDOWN);
            return RiskVerdict::REJECTED_DRAWDOWN;
        }

        // ── 4. Input validation ───────────────────────────────────────────
        if (opp.buy_price <= 0.0 || opp.sell_price <= 0.0 || opp.quantity <= 0.0)
            return RiskVerdict::REJECTED_INVALID_INPUT;
        if (opp.buy_exchange == Exchange::UNKNOWN || opp.sell_exchange == Exchange::UNKNOWN)
            return RiskVerdict::REJECTED_INVALID_INPUT;
        if (opp.buy_exchange == opp.sell_exchange)
            return RiskVerdict::REJECTED_INVALID_INPUT;

        // ── 5. Book freshness (stale data = execution risk) ───────────────
        int64_t buy_age  = opp.local_ts_ns - opp.buy_book_ts_ns;
        int64_t sell_age = opp.local_ts_ns - opp.sell_book_ts_ns;
        if (buy_age  > cfg_.max_book_age_ns || buy_age  < 0 ||
            sell_age > cfg_.max_book_age_ns || sell_age < 0)
            return RiskVerdict::REJECTED_STALE_BOOK;

        // ── 6. Flash crash / book corruption guard ────────────────────────
        if (opp.reference_price > 0.0) {
            auto deviation_bps = [](double price, double ref) noexcept {
                return std::abs((price - ref) / ref) * 10000.0;
            };
            if (deviation_bps(opp.buy_price,  opp.reference_price) > cfg_.max_price_deviation_bps ||
                deviation_bps(opp.sell_price, opp.reference_price) > cfg_.max_price_deviation_bps)
                return RiskVerdict::REJECTED_FLASH_CRASH;
        }

        // ── 7. Gross spread (sell - buy, in bps) ──────────────────────────
        double gross_spread_bps = (opp.sell_price - opp.buy_price) / opp.buy_price * 10000.0;
        if (gross_spread_bps < cfg_.min_spread_bps)
            return RiskVerdict::REJECTED_SPREAD_TOO_SMALL;

        // ── 8. Fee-adjusted profitability ─────────────────────────────────
        double fee_bps = taker_fee(opp.buy_exchange) + taker_fee(opp.sell_exchange);
        double net_spread_bps  = gross_spread_bps - fee_bps;
        double expected_profit = (net_spread_bps / 10000.0) * opp.buy_price * opp.quantity;
        if (expected_profit < cfg_.min_profit_usd)
            return RiskVerdict::REJECTED_FEE_ADJUSTED_LOSS;

        // ── 9. Open leg count ─────────────────────────────────────────────
        if (open_leg_count_.load(std::memory_order_acquire) >= cfg_.max_open_arb_legs)
            return RiskVerdict::REJECTED_MAX_ARB_LEGS;

        // ── 10. Rate limits (fixed-window, lock-free) ──────────────────────
        RiskVerdict rate_verdict = check_and_consume_rate(opp.local_ts_ns);
        if (rate_verdict != RiskVerdict::APPROVED)
            return rate_verdict;

        // ── 11. Position limits ───────────────────────────────────────────
        int sym_idx = find_symbol(opp.symbol);
        int buy_ex  = static_cast<int>(opp.buy_exchange);
        int sell_ex = static_cast<int>(opp.sell_exchange);

        // Treat unknown symbols as zero existing position - still catches oversized new orders
        {
            double pos_buy  = (sym_idx >= 0) ? position_[buy_ex][sym_idx].load(std::memory_order_acquire)  : 0.0;
            double pos_sell = (sym_idx >= 0) ? position_[sell_ex][sym_idx].load(std::memory_order_acquire) : 0.0;

            if (std::abs(pos_buy  + opp.quantity) > cfg_.max_abs_position_per_symbol ||
                std::abs(pos_sell - opp.quantity) > cfg_.max_abs_position_per_symbol)
                return RiskVerdict::REJECTED_POSITION_LIMIT;

            if (sym_idx >= 0) {
                double cross_exp = 0.0;
                for (int e = 0; e < ARB_NUM_EXCHANGES; ++e)
                    cross_exp += std::abs(position_[e][sym_idx].load(std::memory_order_acquire));
                if (cross_exp + opp.quantity > cfg_.max_cross_exchange_exposure)
                    return RiskVerdict::REJECTED_POSITION_LIMIT;
            }
        }

        // ── 12. Notional limits ───────────────────────────────────────────
        double order_notional = opp.buy_price * opp.quantity;

        if (order_notional > cfg_.max_notional_per_symbol)
            return RiskVerdict::REJECTED_NOTIONAL_PER_SYM;

        if (exchange_notional_[buy_ex].load(std::memory_order_acquire)  + order_notional > cfg_.max_notional_per_exchange ||
            exchange_notional_[sell_ex].load(std::memory_order_acquire) + order_notional > cfg_.max_notional_per_exchange)
            return RiskVerdict::REJECTED_NOTIONAL_PER_VENUE;

        if (portfolio_notional_.load(std::memory_order_acquire) + order_notional > cfg_.max_portfolio_notional)
            return RiskVerdict::REJECTED_PORTFOLIO_NOTIONAL;

        return RiskVerdict::APPROVED;
    }

    // ── Leg lifecycle ────────────────────────────────────────────────────────

    // Called immediately after evaluate_arb returns APPROVED.
    // Returns a leg_id (> 0) or 0 if no free slot.
    uint64_t open_leg(const ArbOpportunity& opp) noexcept {
        for (auto& leg : legs_) {
            if (leg.active) continue;

            int buy_ex = static_cast<int>(opp.buy_exchange);
            int sell_ex = static_cast<int>(opp.sell_exchange);
            double notional = opp.buy_price * opp.quantity;

            leg.id                  = next_leg_id_.fetch_add(1, std::memory_order_relaxed);
            leg.buy_exchange        = opp.buy_exchange;
            leg.sell_exchange       = opp.sell_exchange;
            leg.quantity            = opp.quantity;
            leg.expected_buy_price  = opp.buy_price;
            leg.expected_sell_price = opp.sell_price;
            leg.actual_buy_price    = 0.0;
            leg.actual_sell_price   = 0.0;
            leg.open_ts_ns          = opp.local_ts_ns;
            leg.buy_filled          = false;
            leg.sell_filled         = false;
            std::memcpy(leg.symbol, opp.symbol, 15);
            leg.symbol[15]          = '\0';
            leg.active              = true;  // publish last (visibility)

            open_leg_count_.fetch_add(1, std::memory_order_release);

            // Pessimistically add notional on both sides
            add_to(exchange_notional_[buy_ex],  notional);
            add_to(exchange_notional_[sell_ex], notional);
            add_to(portfolio_notional_,         notional);

            LOG_INFO("Arb leg opened",
                     "leg_id", leg.id,
                     "symbol", opp.symbol,
                     "buy_ex",  exchange_to_string(opp.buy_exchange),
                     "sell_ex", exchange_to_string(opp.sell_exchange),
                     "qty",     opp.quantity,
                     "spread_bps",
                     (opp.sell_price - opp.buy_price) / opp.buy_price * 10000.0);

            return leg.id;
        }

        LOG_ERROR("No free arb leg slot", "max_legs", ARB_MAX_OPEN_LEGS);
        return 0;
    }

    // Called when the BUY leg fills (fill thread only)
    void on_buy_filled(uint64_t leg_id, double fill_price, double fill_qty) noexcept {
        ArbLegState* leg = find_leg(leg_id);
        if (!leg) return;

        leg->actual_buy_price = fill_price;
        leg->buy_filled       = true;

        // Update position: long on buy exchange
        int sym_idx = get_or_create_symbol(leg->symbol);
        if (sym_idx >= 0) {
            int buy_ex = static_cast<int>(leg->buy_exchange);
            add_to(position_[buy_ex][sym_idx], fill_qty);
        }

        try_close_leg(*leg);
    }

    // Called when the SELL leg fills (fill thread only)
    void on_sell_filled(uint64_t leg_id, double fill_price, double fill_qty) noexcept {
        ArbLegState* leg = find_leg(leg_id);
        if (!leg) return;

        leg->actual_sell_price = fill_price;
        leg->sell_filled       = true;

        // Update position: short on sell exchange
        int sym_idx = get_or_create_symbol(leg->symbol);
        if (sym_idx >= 0) {
            int sell_ex = static_cast<int>(leg->sell_exchange);
            add_to(position_[sell_ex][sym_idx], -fill_qty);
        }

        try_close_leg(*leg);
    }

    // Called if a leg order is rejected or times out (fill thread only).
    // If the sibling leg already filled, this is an orphaned exposure.
    void on_leg_rejected(uint64_t leg_id) noexcept {
        ArbLegState* leg = find_leg(leg_id);
        if (!leg) return;

        if (leg->buy_filled || leg->sell_filled) {
            // Orphaned position: one side filled, other rejected
            LOG_ERROR("Orphaned arb leg",
                      "leg_id",      leg_id,
                      "symbol",      leg->symbol,
                      "buy_filled",  leg->buy_filled,
                      "sell_filled", leg->sell_filled);

            // Record a conservative loss estimate and trigger circuit breaker check
            double orphan_loss = -(std::abs(leg->expected_buy_price - leg->expected_sell_price) *
                                   leg->quantity);
            record_pnl(orphan_loss);
        }

        release_leg(*leg);
    }

    // ── Heartbeat (call from hot-path loop) ──────────────────────────────────
    void heartbeat() noexcept {
        kill_switch_.heartbeat();
    }

    bool check_heartbeat() noexcept {
        return kill_switch_.check_heartbeat();
    }

    // ── Kill switch control ──────────────────────────────────────────────────
    bool is_kill_switch_active() const noexcept { return kill_switch_.is_active(); }
    void trigger_kill_switch()         noexcept { kill_switch_.trigger(KillReason::MANUAL); }
    void reset_kill_switch()           noexcept { kill_switch_.reset(); }

    // ── Circuit breaker control ──────────────────────────────────────────────
    bool is_circuit_breaker_active() const noexcept {
        return circuit_breaker_active_.load(std::memory_order_acquire);
    }

    void reset_circuit_breaker() noexcept {
        consecutive_losses_.store(0, std::memory_order_release);
        circuit_breaker_active_.store(false, std::memory_order_release);
        LOG_INFO("Circuit breaker reset", "consecutive_losses", 0);
    }

    // ── Metrics ──────────────────────────────────────────────────────────────
    double   get_portfolio_pnl()         const noexcept { return portfolio_pnl_.load(std::memory_order_acquire); }
    double   get_portfolio_notional()    const noexcept { return portfolio_notional_.load(std::memory_order_acquire); }
    uint32_t get_open_leg_count()        const noexcept { return open_leg_count_.load(std::memory_order_acquire); }
    uint32_t get_consecutive_losses()    const noexcept { return consecutive_losses_.load(std::memory_order_acquire); }

    double get_position(Exchange ex, const char* symbol) const noexcept {
        int idx = find_symbol(symbol);
        if (idx < 0) return 0.0;
        int e = static_cast<int>(ex);
        if (e >= ARB_NUM_EXCHANGES) return 0.0;
        return position_[e][idx].load(std::memory_order_acquire);
    }

    double get_exchange_notional(Exchange ex) const noexcept {
        int e = static_cast<int>(ex);
        if (e >= ARB_NUM_EXCHANGES) return 0.0;
        return exchange_notional_[e].load(std::memory_order_acquire);
    }

    KillSwitch& kill_switch() noexcept { return kill_switch_; }
    const ArbRiskConfig& config() const noexcept { return cfg_; }

private:
    ArbRiskConfig cfg_;
    KillSwitch    kill_switch_;

    // Portfolio-wide aggregates
    std::atomic<double>   portfolio_pnl_;
    std::atomic<double>   portfolio_notional_;
    std::atomic<uint32_t> open_leg_count_;
    std::atomic<uint32_t> consecutive_losses_;
    std::atomic<bool>     circuit_breaker_active_;

    // Rate limiting (fixed-window per second and per minute)
    std::atomic<uint32_t> rate_second_count_;
    std::atomic<uint32_t> rate_minute_count_;
    std::atomic<int64_t>  rate_second_start_ns_;
    std::atomic<int64_t>  rate_minute_start_ns_;

    // Per-exchange notional
    std::atomic<double> exchange_notional_[ARB_NUM_EXCHANGES];

    // Symbol registry (linear scan; typically < 20 symbols in live arb)
    char symbols_[ARB_MAX_SYMBOLS][16];
    int  symbol_count_;

    // Per-exchange, per-symbol position (updated by fill thread)
    std::atomic<double> position_[ARB_NUM_EXCHANGES][ARB_MAX_SYMBOLS];

    // Open arb legs pool
    ArbLegState           legs_[ARB_MAX_OPEN_LEGS];
    std::atomic<uint64_t> next_leg_id_;

    // ── Helpers ──────────────────────────────────────────────────────────────

    double taker_fee(Exchange ex) const noexcept {
        int e = static_cast<int>(ex);
        return (e < ARB_NUM_EXCHANGES) ? cfg_.taker_fee_bps[e] : 20.0;
    }

    // Fixed-window rate limiter. Allows slight over-counting at window boundaries
    // in exchange for zero locks. Acceptable for trading rate limits.
    RiskVerdict check_and_consume_rate(int64_t ts_ns) noexcept {
        static constexpr int64_t ONE_SECOND = 1'000'000'000LL;
        static constexpr int64_t ONE_MINUTE = 60'000'000'000LL;

        // Per-second window
        int64_t sec_start = rate_second_start_ns_.load(std::memory_order_relaxed);
        if (ts_ns - sec_start >= ONE_SECOND) {
            if (rate_second_start_ns_.compare_exchange_strong(
                    sec_start, ts_ns, std::memory_order_relaxed))
                rate_second_count_.store(0, std::memory_order_relaxed);
        }
        if (rate_second_count_.fetch_add(1, std::memory_order_relaxed) >= cfg_.max_orders_per_second) {
            rate_second_count_.fetch_sub(1, std::memory_order_relaxed);
            return RiskVerdict::REJECTED_RATE_LIMIT;
        }

        // Per-minute window
        int64_t min_start = rate_minute_start_ns_.load(std::memory_order_relaxed);
        if (ts_ns - min_start >= ONE_MINUTE) {
            if (rate_minute_start_ns_.compare_exchange_strong(
                    min_start, ts_ns, std::memory_order_relaxed))
                rate_minute_count_.store(0, std::memory_order_relaxed);
        }
        if (rate_minute_count_.fetch_add(1, std::memory_order_relaxed) >= cfg_.max_orders_per_minute) {
            rate_minute_count_.fetch_sub(1, std::memory_order_relaxed);
            rate_second_count_.fetch_sub(1, std::memory_order_relaxed);
            return RiskVerdict::REJECTED_RATE_LIMIT;
        }

        return RiskVerdict::APPROVED;
    }

    // Lock-free atomic add for double (load-store; safe for single-writer fill thread)
    static void add_to(std::atomic<double>& a, double delta) noexcept {
        double old = a.load(std::memory_order_acquire);
        a.store(old + delta, std::memory_order_release);
    }

    void record_pnl(double pnl) noexcept {
        double old = portfolio_pnl_.load(std::memory_order_acquire);
        portfolio_pnl_.store(old + pnl, std::memory_order_release);

        if (pnl <= cfg_.circuit_breaker_loss_usd) {
            uint32_t losses = consecutive_losses_.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (losses >= cfg_.circuit_breaker_count) {
                circuit_breaker_active_.store(true, std::memory_order_release);
                LOG_ERROR("Circuit breaker tripped",
                          "consecutive_losses", losses,
                          "pnl_usd", portfolio_pnl_.load(std::memory_order_relaxed));
            }
        } else if (pnl > 0.0) {
            consecutive_losses_.store(0, std::memory_order_release);
        }

        if (portfolio_pnl_.load(std::memory_order_acquire) < cfg_.max_drawdown_usd)
            kill_switch_.trigger(KillReason::DRAWDOWN);
    }

    void try_close_leg(ArbLegState& leg) noexcept {
        if (!leg.buy_filled || !leg.sell_filled) return;

        double gross_pnl = (leg.actual_sell_price - leg.actual_buy_price) * leg.quantity;
        double fee_cost  = ((taker_fee(leg.buy_exchange) + taker_fee(leg.sell_exchange))
                            / 10000.0) * leg.actual_buy_price * leg.quantity;
        double net_pnl   = gross_pnl - fee_cost;
        record_pnl(net_pnl);

        LOG_INFO("Arb leg closed",
                 "leg_id",    leg.id,
                 "symbol",    leg.symbol,
                 "net_pnl",   net_pnl,
                 "gross_pnl", gross_pnl);

        release_leg(leg);
    }

    void release_leg(ArbLegState& leg) noexcept {
        double notional = leg.expected_buy_price * leg.quantity;
        int buy_ex  = static_cast<int>(leg.buy_exchange);
        int sell_ex = static_cast<int>(leg.sell_exchange);

        // Clamp to zero to guard against floating-point drift
        auto subtract = [](std::atomic<double>& a, double v) noexcept {
            double old = a.load(std::memory_order_acquire);
            a.store(std::max(0.0, old - v), std::memory_order_release);
        };

        subtract(exchange_notional_[buy_ex],  notional);
        subtract(exchange_notional_[sell_ex], notional);
        subtract(portfolio_notional_,         notional);

        open_leg_count_.fetch_sub(1, std::memory_order_release);
        leg.active = false;
    }

    int find_symbol(const char* sym) const noexcept {
        for (int i = 0; i < symbol_count_; ++i) {
            if (std::strncmp(symbols_[i], sym, 15) == 0) return i;
        }
        return -1;
    }

    int get_or_create_symbol(const char* sym) noexcept {
        int idx = find_symbol(sym);
        if (idx >= 0) return idx;
        if (symbol_count_ >= ARB_MAX_SYMBOLS) {
            LOG_ERROR("Symbol table full", "max_symbols", ARB_MAX_SYMBOLS);
            return -1;
        }
        std::memcpy(symbols_[symbol_count_], sym, 15);
        symbols_[symbol_count_][15] = '\0';
        return symbol_count_++;
    }

    ArbLegState* find_leg(uint64_t leg_id) noexcept {
        for (auto& leg : legs_) {
            if (leg.active && leg.id == leg_id) return &leg;
        }
        LOG_WARN("Leg not found", "leg_id", leg_id);
        return nullptr;
    }
};

}  // namespace trading
