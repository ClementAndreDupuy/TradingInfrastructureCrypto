============================================================
  Shadow Validation Report
============================================================

── Order execution ─────────────────────────────────────────
  Total orders submitted : 2
  Fills                  : 2
  Cancels                : 0
  Rejects                : 0
  Resting (open)         : 0
  Fill rate              : 100.00%
  Net P&L                : $-0.1700
  Fee drag               : $-0.0700
  Max drawdown           : $-100.2500
  Total volume (qty)     : 2.000000
  Impl shortfall         : 0.5000 bps
  Fill-to-markout        : -2.7500 bps
  Edge at entry          : 4.7500 bps
  Spread paid/captured   : 10.0000 / 5.0000 bps
  Avg hold / inv age     : 500.00 / 1000.00 ms
  Worst venue            : BINANCE

── Per-exchange breakdown ───────────────────────────────────
  BINANCE     fills=1  maker=0%  fees=$0.0500  shortfall=2.000bps  markout=-3.000bps  hold=200.0ms  contrib=$-0.1001
  OKX         fills=1  maker=100%  fees=$0.0200  shortfall=-1.000bps  markout=-2.500bps  hold=800.0ms  contrib=$-0.0350

── Loss attribution ────────────────────────────────────────
  Fees                  : $-0.0700
  Slippage              : -0.0050 bps
  Adverse selection     : 2.7500 bps
  Stale inventory ratio : 0.00%

── Neural alpha signal ──────────────────────────────────────
  Signals generated      : 2
  Session duration       : 0.0 min
  Mean effective signal  : 2.7500 bps
  Effective signal std   : 5.2500 bps
  Mean raw signal        : 2.7500 bps
  Raw signal std         : 5.2500 bps
  Avg risk score         : 0.2500
  IC (rolling 20)        : 0.0000
  ICIR (annualised)      : 0.0000

── Gating breakdown ────────────────────────────────────────
  Confidence gate        : 0
  Horizon disagreement   : 0
  Safe-mode gate         : 0

── Timestamp quality ───────────────────────────────────────
  Exchange ts missing    : 0
  Local ts missing       : 0
  Exchange non-monotonic : 0
  Local non-monotonic    : 0
  Event-index regressions: 0
  Venue timestamp jumps  : 0

── Shadow health ────────────────────────────────────────────
  Training events        : 0
  Runtime incidents      : 0
  Drift breaches         : 0
  Canary rollbacks       : 0
  Safe-mode activations  : 0
  Retrain completions    : 0
  Retrain failures       : 0

── Venue quality ────────────────────────────────────────────
  BINANCE     received=10  used=10  missing=0  rest_fallback=0  resnapshot=0
  OKX         received=10  used=10  missing=0  rest_fallback=0  resnapshot=0

── Readiness check ─────────────────────────────────────────
  Run >= 2 weeks before promoting to live.
  Shadow fill rates must match backtest within 10%.
  Kill switch drill must be completed.
  All monitoring dashboards must be operational.

============================================================