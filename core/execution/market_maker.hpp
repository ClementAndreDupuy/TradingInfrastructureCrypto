#pragma once

#include "../common/logging.hpp"
#include "../feeds/common/book_manager.hpp"
#include "../ipc/alpha_signal.hpp"
#include "../ipc/regime_signal.hpp"
#include "../risk/circuit_breaker.hpp"
#include "../risk/kill_switch.hpp"
#include "common/order_manager.hpp"

#include <chrono>
#include <cmath>
#include <cstring>

namespace trading {

struct MarketMakerConfig {
    char symbol[16] = "BTCUSDT";
    Exchange exchange = Exchange::BINANCE;

    double half_spread_bps = 2.0;
    double skew_factor = 0.3;
    double max_skew_bps = 5.0;
    double order_qty = 0.001;
    double max_position = 0.01;

    double requote_bps = 1.0;
    double requote_signal = 2.0;

    double stop_loss_bps = 20.0;
    double limit_slip_bps = 3.0;

    double risk_max = 0.65;
    double risk_widen_bps = 3.0;
    double signal_min_bps = 1.5;
    double inventory_skew_decay_power = 2.0;

    double shock_block_threshold = 0.75;
    double illiquid_block_threshold = 0.80;
    double regime_widen_bps = 2.0;

    int64_t stale_ns = 2'000'000'000LL;
};

class NeuralAlphaMarketMaker {
  public:
    NeuralAlphaMarketMaker(OrderManager& order_mgr, const BookManager& book, KillSwitch& kill,
                           const MarketMakerConfig& cfg)
        : NeuralAlphaMarketMaker(order_mgr, book, kill, nullptr, cfg) {}

    NeuralAlphaMarketMaker(OrderManager& order_mgr, const BookManager& book, KillSwitch& kill,
                           CircuitBreaker* circuit_breaker = nullptr,
                           const MarketMakerConfig& cfg = {})
        : om_(order_mgr), book_(book), kill_(kill), circuit_breaker_(circuit_breaker), cfg_(cfg) {

        om_.on_fill = [this](const ManagedOrder& mo, const FillUpdate& u) { on_fill(mo, u); };
    }

    void on_book_update() {
        if (kill_.is_active())
            return;
        om_.drain_fills();
        if (!book_.is_ready())
            return;

        AlphaSignal sig = alpha_reader_.read();
        RegimeSignal regime = regime_reader_.read();
        const double mid = book_.mid_price();
        if (mid <= 0.0)
            return;
        om_.update_mid_price(cfg_.symbol, cfg_.exchange, mid);

        check_stop(mid, sig);

        const bool mid_moved = std::abs(mid - last_quoted_mid_) / mid * 1e4 >= cfg_.requote_bps;
        const double sig_bps = sig.signal_bps;
        const bool sig_changed = std::abs(sig_bps - last_signal_bps_) >= cfg_.requote_signal;

        if (!bid_id_ && !ask_id_) {
            post_quotes(mid, sig, regime);
        } else if (mid_moved || sig_changed) {
            cancel_quotes();
            post_quotes(mid, sig, regime);
        }
    }

    void set_alpha_signal(double signal_bps, double risk_score) {
        injected_signal_bps_ = signal_bps;
        injected_risk_score_ = risk_score;
        injected_signal_set_ = true;
    }

    double position() const noexcept { return om_.position(); }
    double entry_price() const noexcept { return entry_price_; }
    double realized_pnl() const noexcept { return om_.realized_pnl(); }

  private:
    OrderManager& om_;
    const BookManager& book_;
    KillSwitch& kill_;
    CircuitBreaker* circuit_breaker_ = nullptr;
    MarketMakerConfig cfg_;
    AlphaSignalReader alpha_reader_;
    RegimeSignalReader regime_reader_;

    uint64_t bid_id_ = 0;
    uint64_t ask_id_ = 0;
    uint64_t stop_id_ = 0;
    double last_quoted_mid_ = 0.0;
    double last_signal_bps_ = 0.0;

    double entry_price_ = 0.0;
    double injected_signal_bps_ = 0.0;
    double injected_risk_score_ = 0.0;
    bool injected_signal_set_ = false;

    void post_quotes(double mid, const AlphaSignal& sig, const RegimeSignal& regime) {
        double net_pos = om_.position();

        double signal_bps = injected_signal_set_ ? injected_signal_bps_ : sig.signal_bps;
        double risk_score = injected_signal_set_ ? injected_risk_score_ : sig.risk_score;
        bool stale = is_stale(sig);

        if (stale) {
            signal_bps = 0.0;
            risk_score = 0.0;
        }

        const bool regime_stale = regime_reader_.is_stale(regime, cfg_.stale_ns);
        const double p_shock = regime_stale ? 0.0 : regime.p_shock;
        const double p_illiquid = regime_stale ? 0.0 : regime.p_illiquid;

        if (p_shock >= cfg_.shock_block_threshold || p_illiquid >= cfg_.illiquid_block_threshold) {
            cancel_quotes();
            return;
        }

        double skew = 0.0;
        if (std::abs(signal_bps) >= cfg_.signal_min_bps)
            skew = std::max(-cfg_.max_skew_bps,
                            std::min(cfg_.max_skew_bps, cfg_.skew_factor * signal_bps));

        double decay = 1.0;
        if (cfg_.max_position > 0.0) {
            double inv_ratio = std::abs(net_pos) / cfg_.max_position;
            inv_ratio = std::max(0.0, std::min(1.0, inv_ratio));
            const double decay_power = std::max(1.0, cfg_.inventory_skew_decay_power);
            decay = std::pow(inv_ratio, decay_power);
            skew *= decay;
        }

        double extra = (risk_score >= cfg_.risk_max) ? cfg_.risk_widen_bps : 0.0;
        extra += cfg_.regime_widen_bps * (p_shock + 0.5 * p_illiquid);

        double half_bid = cfg_.half_spread_bps - skew + extra;
        double half_ask = cfg_.half_spread_bps + skew + extra;

        half_bid = std::max(half_bid, 0.5);
        half_ask = std::max(half_ask, 0.5);

        double bid_px = mid * (1.0 - half_bid * 1e-4);
        double ask_px = mid * (1.0 + half_ask * 1e-4);

        double qty = cfg_.order_qty;
        bool post_bid = (net_pos + qty) <= cfg_.max_position;
        bool post_ask = (net_pos - qty) >= -cfg_.max_position;

        if (post_bid)
            bid_id_ = submit_limit(Side::BID, bid_px, qty);
        if (post_ask)
            ask_id_ = submit_limit(Side::ASK, ask_px, qty);

        last_quoted_mid_ = mid;
        last_signal_bps_ = signal_bps;
    }

    void cancel_quotes() {
        if (bid_id_) {
            om_.cancel(bid_id_);
            bid_id_ = 0;
        }
        if (ask_id_) {
            om_.cancel(ask_id_);
            ask_id_ = 0;
        }
    }

    void check_stop(double mid, const AlphaSignal& /*sig*/) {
        double pos = om_.position();
        if (pos == 0.0 || stop_id_ != 0)
            return;

        if (entry_price_ <= 0.0)
            return;

        double unreal_bps = (pos > 0.0) ? (mid - entry_price_) / entry_price_ * 1e4
                                        : (entry_price_ - mid) / entry_price_ * 1e4;

        if (unreal_bps < -cfg_.stop_loss_bps) {
            LOG_WARN("Stop-limit triggered", "position", pos, "entry_px", entry_price_, "mid", mid,
                     "unreal_bps", unreal_bps);

            cancel_quotes();

            if (pos > 0.0) {
                double limit_px = mid * (1.0 - cfg_.limit_slip_bps * 1e-4);
                stop_id_ = submit_stop_limit(Side::ASK, mid, limit_px, std::abs(pos));
            } else {
                double limit_px = mid * (1.0 + cfg_.limit_slip_bps * 1e-4);
                stop_id_ = submit_stop_limit(Side::BID, mid, limit_px, std::abs(pos));
            }
        }
    }

    static void copy_symbol(char (&dst)[16], const char (&src)[16]) noexcept {
        std::memcpy(dst, src, sizeof(dst));
        dst[sizeof(dst) - 1] = '\0';
    }

    uint64_t submit_limit(Side side, double price, double qty) {
        if (!run_pre_submit_checks(price))
            return 0;

        Order o;
        copy_symbol(o.symbol, cfg_.symbol);
        o.exchange = cfg_.exchange;
        o.side = side;
        o.type = OrderType::LIMIT;
        o.tif = TimeInForce::GTX;
        o.price = price;
        o.quantity = qty;
        return om_.submit(o);
    }

    uint64_t submit_stop_limit(Side side, double stop_px, double limit_px, double qty) {
        if (!run_pre_submit_checks(limit_px))
            return 0;

        Order o;
        copy_symbol(o.symbol, cfg_.symbol);
        o.exchange = cfg_.exchange;
        o.side = side;
        o.type = OrderType::STOP_LIMIT;
        o.tif = TimeInForce::IOC;
        o.stop_price = stop_px;
        o.price = limit_px;
        o.quantity = qty;
        return om_.submit(o);
    }

    void on_fill(const ManagedOrder& mo, const FillUpdate& u) {
        if (u.new_state != OrderState::FILLED) {
            if (mo.order.client_order_id == bid_id_)
                bid_id_ = 0;
            if (mo.order.client_order_id == ask_id_)
                ask_id_ = 0;
            if (mo.order.client_order_id == stop_id_)
                stop_id_ = 0;
            return;
        }

        const bool is_bid = mo.order.side == Side::BID;
        const bool is_stop_fill = (mo.order.client_order_id == stop_id_);
        const double stop_entry_px = entry_price_;
        const double qty = u.fill_qty;
        const double px = u.fill_price;
        const double pos = om_.position();

        const double signed_fill_qty = is_bid ? qty : -qty;
        const double prev_pos = pos - signed_fill_qty;

        if (pos == 0.0) {
            entry_price_ = 0.0;
        } else if (prev_pos == 0.0 || entry_price_ <= 0.0) {
            entry_price_ = px;
        } else if ((prev_pos > 0.0 && signed_fill_qty > 0.0) ||
                   (prev_pos < 0.0 && signed_fill_qty < 0.0)) {
            const double old_qty = std::abs(prev_pos);
            const double new_qty = std::abs(signed_fill_qty);
            entry_price_ = ((entry_price_ * old_qty) + (px * new_qty)) / (old_qty + new_qty);
        }

        if (mo.order.client_order_id == stop_id_) {
            stop_id_ = 0;
            entry_price_ = 0.0;
            LOG_INFO("Stop-limit filled — position flattened", "fill_px", px);
        }

        if (mo.order.client_order_id == bid_id_)
            bid_id_ = 0;
        if (mo.order.client_order_id == ask_id_)
            ask_id_ = 0;

        if (circuit_breaker_ && is_stop_fill) {
            const double realized_leg_pnl = (mo.order.side == Side::ASK)
                                                ? (px - stop_entry_px) * qty
                                                : (stop_entry_px - px) * qty;
            circuit_breaker_->record_leg_result(realized_leg_pnl);
        }

        LOG_INFO("Fill", "side", is_bid ? "BID" : "ASK", "qty", qty, "px", px, "net_pos", pos);
    }

    bool run_pre_submit_checks(double order_price) {
        if (!circuit_breaker_)
            return true;

        auto rate = circuit_breaker_->check_order_rate();
        if (rate != CircuitCheckResult::OK)
            return false;

        auto stale = circuit_breaker_->check_book_age(book_);
        if (stale != CircuitCheckResult::OK)
            return false;

        auto drawdown = circuit_breaker_->check_drawdown(om_.realized_pnl());
        if (drawdown != CircuitCheckResult::OK)
            return false;

        auto deviation = circuit_breaker_->check_price_deviation(order_price);
        if (deviation != CircuitCheckResult::OK)
            return false;

        auto losses = circuit_breaker_->check_consecutive_losses();
        if (losses != CircuitCheckResult::OK)
            return false;

        return true;
    }

    bool is_stale(const AlphaSignal& s) const noexcept {
        if (s.ts_ns == 0)
            return true;
        using namespace std::chrono;
        int64_t now = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
        return (now - s.ts_ns) > cfg_.stale_ns;
    }
};

}
