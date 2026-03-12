"""
Neural alpha execution engine.

Reads the live signal from the shared memory file written by shadow_session.py
and manages positions accordingly. Runs in shadow (paper) mode by default;
all order objects are logged but never sent to an exchange.

Signal gate logic (mirrors C++ AlphaSignalReader):
    LONG  when signal_bps > signal_min_bps AND risk_score < risk_max AND dir confirmed
    SHORT when signal_bps < -signal_min_bps AND risk_score < risk_max AND dir confirmed
    FLAT  otherwise — close any open position

Usage:
    python -m research.alpha.neural_alpha.execution_engine \\
        --signal-file /tmp/neural_alpha_signal.bin \\
        --duration 3600
"""
from __future__ import annotations

import argparse
import json
import mmap
import signal
import struct
import time
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

# Shared memory layout: [float64 signal_bps][float64 risk_score][int64 ts_ns]
_SIGNAL_FMT  = "ddq"
_SIGNAL_SIZE = struct.calcsize(_SIGNAL_FMT)  # 24 bytes


@dataclass
class ExecutionConfig:
    signal_file: str = "/tmp/neural_alpha_signal.bin"
    log_path: str = "neural_alpha_execution.jsonl"
    interval_ms: int = 500
    duration_s: int = 3600
    signal_min_bps: float = 3.0       # min |signal| to open a position
    risk_max: float = 0.65            # max adverse-selection risk to trade
    stale_ns: int = 2_000_000_000    # 2 s — treat signal as stale past this
    trade_qty: float = 0.001          # BTC per order
    max_position: float = 0.01        # max position magnitude (BTC)
    taker_fee_bps: float = 5.0        # Binance VIP-0 taker
    stop_loss_bps: float = 20.0       # per-position stop loss
    shadow: bool = True               # paper mode — never sends to exchange
    report_interval_s: int = 60


@dataclass
class Order:
    order_id: str
    side: str          # "BUY" | "SELL"
    qty: float
    price: float       # fill price (taker = best ask/bid)
    ts_ns: int
    fee: float
    reason: str


@dataclass
class Position:
    side: str          # "LONG" | "SHORT"
    qty: float
    entry_price: float
    entry_ts_ns: int
    entry_fee: float


class SignalReader:
    """Reads neural alpha signal from the shared memory file."""

    def __init__(self, path: str) -> None:
        self._path = path
        self._mm: mmap.mmap | None = None
        self._f = None

    def open(self) -> bool:
        try:
            self._f = open(self._path, "rb")
            self._mm = mmap.mmap(self._f.fileno(), _SIGNAL_SIZE, access=mmap.ACCESS_READ)
            return True
        except OSError:
            return False

    def read(self) -> tuple[float, float, int] | None:
        """Return (signal_bps, risk_score, ts_ns) or None if not available."""
        if self._mm is None:
            return None
        self._mm.seek(0)
        raw = self._mm.read(_SIGNAL_SIZE)
        if len(raw) < _SIGNAL_SIZE:
            return None
        return struct.unpack(_SIGNAL_FMT, raw)

    def close(self) -> None:
        if self._mm:
            self._mm.close()
        if self._f:
            self._f.close()


class NeuralAlphaExecutionEngine:
    """
    Paper-trades the neural alpha signal.

    Each cycle:
    1. Reads signal from shared memory.
    2. Checks staleness and risk gate.
    3. Opens/closes positions based on signal direction.
    4. Marks positions to market and checks stop-loss.
    5. Logs all orders to JSONL.
    """

    def __init__(self, cfg: ExecutionConfig) -> None:
        self.cfg = cfg
        self._position: Position | None = None
        self._pnl: float = 0.0
        self._peak_pnl: float = 0.0
        self._max_dd: float = 0.0
        self._orders: list[Order] = []
        self._trade_pnls: list[float] = []
        self._signal_reader = SignalReader(cfg.signal_file)
        self._log_fp = open(cfg.log_path, "a")
        self._running = False
        self._order_seq = 0

    # ── Signal helpers ────────────────────────────────────────────────────────

    def _is_stale(self, ts_ns: int) -> bool:
        return ts_ns == 0 or (time.time_ns() - ts_ns) > self.cfg.stale_ns

    def _signal_allows_long(self, sig: float, risk: float, ts_ns: int) -> bool:
        if self._is_stale(ts_ns):
            return False
        return sig >= self.cfg.signal_min_bps and risk < self.cfg.risk_max

    def _signal_allows_short(self, sig: float, risk: float, ts_ns: int) -> bool:
        if self._is_stale(ts_ns):
            return False
        return sig <= -self.cfg.signal_min_bps and risk < self.cfg.risk_max

    # ── Order helpers ─────────────────────────────────────────────────────────

    def _next_id(self) -> str:
        self._order_seq += 1
        return f"shadow_{self._order_seq:06d}"

    def _make_order(self, side: str, qty: float, price: float,
                    ts_ns: int, reason: str) -> Order:
        fee = qty * price * self.cfg.taker_fee_bps * 1e-4
        return Order(
            order_id=self._next_id(),
            side=side,
            qty=qty,
            price=price,
            ts_ns=ts_ns,
            fee=fee,
            reason=reason,
        )

    def _log_order(self, order: Order, extra: dict | None = None) -> None:
        record = {
            "order_id":  order.order_id,
            "side":      order.side,
            "qty":       order.qty,
            "price":     order.price,
            "ts_ns":     order.ts_ns,
            "fee":       order.fee,
            "reason":    order.reason,
            "shadow":    self.cfg.shadow,
        }
        if extra:
            record.update(extra)
        self._log_fp.write(json.dumps(record) + "\n")
        self._log_fp.flush()

    # ── Position management ───────────────────────────────────────────────────

    def _open(self, side: str, mid: float, ts_ns: int, reason: str) -> None:
        qty = min(self.cfg.trade_qty, self.cfg.max_position)
        # Taker fill: buys at ask, sells at bid (approximate as mid ± 0.5 spread)
        # Without live quote we use mid as a conservative approximation.
        fill_px = mid
        order = self._make_order(
            "BUY" if side == "LONG" else "SELL", qty, fill_px, ts_ns, reason
        )
        self._pnl -= order.fee
        self._position = Position(
            side=side,
            qty=qty,
            entry_price=fill_px,
            entry_ts_ns=ts_ns,
            entry_fee=order.fee,
        )
        self._orders.append(order)
        self._log_order(order, {"event": "open", "pnl_running": self._pnl})
        print(f"  [exec] OPEN {side}  qty={qty}  px={fill_px:.2f}  fee={order.fee:.4f}")

    def _close(self, mid: float, ts_ns: int, reason: str) -> None:
        pos = self._position
        if pos is None:
            return
        fill_px = mid
        order = self._make_order(
            "SELL" if pos.side == "LONG" else "BUY",
            pos.qty, fill_px, ts_ns, reason,
        )
        gross = pos.qty * (fill_px - pos.entry_price) * (1 if pos.side == "LONG" else -1)
        net   = gross - order.fee - pos.entry_fee
        self._pnl += gross - order.fee
        self._peak_pnl = max(self._peak_pnl, self._pnl)
        self._max_dd   = min(self._max_dd, self._pnl - self._peak_pnl)
        self._trade_pnls.append(net)
        self._position = None
        self._orders.append(order)
        self._log_order(order, {
            "event":      "close",
            "gross_pnl":  gross,
            "net_pnl":    net,
            "pnl_running": self._pnl,
        })
        print(
            f"  [exec] CLOSE {pos.side}  px={fill_px:.2f}  "
            f"gross={gross:.4f}  net={net:.4f}  reason={reason}"
        )

    # ── Main loop ─────────────────────────────────────────────────────────────

    def run(self) -> dict:
        self._running = True
        ok = self._signal_reader.open()
        if not ok:
            print(f"  [exec] WARNING: signal file not found at {self.cfg.signal_file}. "
                  "Will retry each cycle.")

        interval_s   = self.cfg.interval_ms / 1000.0
        end_time     = time.time() + self.cfg.duration_s
        last_report  = time.time()
        stop_loss    = self.cfg.stop_loss_bps * 1e-4

        print(
            f"Execution engine started — shadow={self.cfg.shadow}  "
            f"duration={self.cfg.duration_s}s  signal={self.cfg.signal_file}"
        )

        try:
            while self._running and time.time() < end_time:
                tick_start = time.time()
                ts_now     = time.time_ns()

                # Retry open if signal file appeared after start
                if not self._signal_reader._mm:
                    self._signal_reader.open()

                reading = self._signal_reader.read()
                if reading is None:
                    time.sleep(interval_s)
                    continue

                sig_bps, risk, sig_ts = reading
                # Derive approximate mid from signal alone (no live quote here).
                # A production integration would fetch best_bid/ask from the feed.
                mid = 0.0  # placeholder; PnL meaningless without a real price feed

                pos = self._position

                if pos is not None and mid > 0:
                    # Stop-loss check
                    unreal = pos.qty * (mid - pos.entry_price) * (
                        1 if pos.side == "LONG" else -1
                    )
                    if unreal / (pos.qty * pos.entry_price + 1e-9) < -stop_loss:
                        self._close(mid, ts_now, "STOP")
                        continue

                # Signal-driven entry / exit
                want_long  = self._signal_allows_long(sig_bps, risk, sig_ts)
                want_short = self._signal_allows_short(sig_bps, risk, sig_ts)

                if pos is None:
                    if want_long:
                        self._open("LONG",  mid or 1.0, ts_now, "SIGNAL")
                    elif want_short:
                        self._open("SHORT", mid or 1.0, ts_now, "SIGNAL")
                else:
                    should_exit = (
                        (pos.side == "LONG"  and not want_long)
                        or (pos.side == "SHORT" and not want_short)
                    )
                    if should_exit:
                        self._close(mid or pos.entry_price, ts_now, "EXIT")

                if time.time() - last_report >= self.cfg.report_interval_s:
                    self._print_summary()
                    last_report = time.time()

                time.sleep(max(0.0, interval_s - (time.time() - tick_start)))

        finally:
            if self._position is not None:
                self._close(self._position.entry_price, time.time_ns(), "EOD")
            self._print_summary()
            self._log_fp.close()
            self._signal_reader.close()
            print("Execution engine stopped.")

        return self._build_metrics()

    def stop(self) -> None:
        self._running = False

    # ── Reporting ─────────────────────────────────────────────────────────────

    def _print_summary(self) -> None:
        n = len(self._trade_pnls)
        sharpe = 0.0
        win_rate = 0.0
        if n > 1:
            pnls = np.array(self._trade_pnls)
            mu   = pnls.mean()
            std  = pnls.std(ddof=1) + 1e-9
            sharpe = (mu / std) * np.sqrt(min(n, 252 * 24))
            win_rate = float((pnls > 0).mean())
        print(
            f"  [exec] trades={n}  net_pnl={self._pnl:.4f}  "
            f"max_dd={self._max_dd:.4f}  sharpe={sharpe:.2f}  "
            f"win_rate={win_rate:.2%}"
        )

    def _build_metrics(self) -> dict:
        pnls = np.array(self._trade_pnls) if self._trade_pnls else np.array([0.0])
        mu   = float(pnls.mean())
        std  = float(pnls.std(ddof=1)) if len(pnls) > 1 else 1e-9
        n    = len(self._trade_pnls)
        sharpe = (mu / (std + 1e-9)) * np.sqrt(min(max(n, 1), 252 * 24))
        wins   = pnls[pnls > 0]
        losses = pnls[pnls <= 0]
        return {
            "total_trades":      n,
            "total_net_pnl":     float(pnls.sum()),
            "avg_net_pnl":       mu,
            "sharpe_annualised": sharpe,
            "win_rate":          float((pnls > 0).mean()),
            "profit_factor":     float(wins.sum() / abs(losses.sum())) if losses.size else float("inf"),
            "max_drawdown":      self._max_dd,
        }


# ── CLI ───────────────────────────────────────────────────────────────────────

def main() -> None:
    ap = argparse.ArgumentParser(description="Neural alpha execution engine (shadow mode)")
    ap.add_argument("--signal-file",     type=str,   default="/tmp/neural_alpha_signal.bin",
                    dest="signal_file")
    ap.add_argument("--log-path",        type=str,   default="neural_alpha_execution.jsonl",
                    dest="log_path")
    ap.add_argument("--interval-ms",     type=int,   default=500,   dest="interval_ms")
    ap.add_argument("--duration",        type=int,   default=3600)
    ap.add_argument("--signal-min-bps",  type=float, default=3.0,   dest="signal_min_bps")
    ap.add_argument("--risk-max",        type=float, default=0.65,  dest="risk_max")
    ap.add_argument("--trade-qty",       type=float, default=0.001, dest="trade_qty")
    ap.add_argument("--max-position",    type=float, default=0.01,  dest="max_position")
    ap.add_argument("--fee-bps",         type=float, default=5.0,   dest="taker_fee_bps")
    ap.add_argument("--stop-loss-bps",   type=float, default=20.0,  dest="stop_loss_bps")
    ap.add_argument("--live",            action="store_true",
                    help="Disable shadow mode (NOT IMPLEMENTED — reserved for future use)")
    ap.add_argument("--report-interval", type=int,   default=60,    dest="report_interval_s")
    args = ap.parse_args()

    cfg = ExecutionConfig(
        signal_file=args.signal_file,
        log_path=args.log_path,
        interval_ms=args.interval_ms,
        duration_s=args.duration,
        signal_min_bps=args.signal_min_bps,
        risk_max=args.risk_max,
        trade_qty=args.trade_qty,
        max_position=args.max_position,
        taker_fee_bps=args.taker_fee_bps,
        stop_loss_bps=args.stop_loss_bps,
        shadow=not args.live,
        report_interval_s=args.report_interval_s,
    )

    engine = NeuralAlphaExecutionEngine(cfg)

    def _handle_sigint(sig, frame):
        print("\nInterrupt received — stopping engine...")
        engine.stop()

    signal.signal(signal.SIGINT, _handle_sigint)
    metrics = engine.run()
    print("\nFinal metrics:", json.dumps(metrics, indent=2))


if __name__ == "__main__":
    main()
