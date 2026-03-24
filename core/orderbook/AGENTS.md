# core/orderbook/

Order book state container used by feed handlers and execution/risk readers.

## Class & Methods (Quick Reference)

- **`OrderBook` (`orderbook.hpp`)** — Flat-array bid/ask grid with snapshot + delta application.
  - `apply_snapshot(const Snapshot&)`: Validates and replaces current book state from full snapshot input.
  - `apply_delta(const Delta&)`: Applies one price-level mutation for bid/ask side.
  - `get_best_bid()/get_best_ask()/get_mid_price()/get_spread()`: Top-of-book and derived pricing helpers.
  - `get_top_levels(size_t, ...)`: Exports best N levels per side for downstream consumers.
  - `tick_size()/max_levels()/active_levels()/base_price()`: Exposes grid sizing/bounds metadata.
