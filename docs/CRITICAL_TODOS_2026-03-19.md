# Critical TODOs — Execution Connector Exchange-Doc Audit

**Created:** 2026-03-19  
**Scope:** Critical remediation items identified during the execution-connector audit for Binance, Kraken, OKX, and Coinbase.

This file isolates the execution-layer live-trading blockers from the main rolling TODO list. The full analysis lives in `docs/reports/AUDIT_REPORT_2026-03-19.md`.

---

## Critical execution remediation items

- [x] **C2** `core/execution/binance/binance_connector.cpp` + `core/execution/live_connector_base.hpp` — **Binance live order-entry contract is out of spec with current Spot docs**
  The connector currently uses a header-based auth/signature model, omits documented required fields like `timeInForce` for limit orders, queries/cancels without `symbol`, uses a non-documented replace path, and requests `myTrades` without the required `symbol`. Treat the Binance execution connector as non-deployable until it is rebuilt against the current Spot API contract.
  - acceptance criteria:
    - [x] Signed REST calls follow the current Binance Spot request-security contract (`timestamp`/`signature` and any required signed params in the documented location)
    - [x] `submit_order` sends the documented mandatory fields for each order type, including `timeInForce` for limit orders and venue-compatible client-order identifiers
    - [x] `cancel_order`, `query_order`, and trade-history reconciliation include the documented symbol-scoped parameters and pass fixture coverage against official examples
    - [x] `replace_order` is reworked onto Binance's currently documented modify flow (`cancelReplace` and/or `amend/keepPriority`) with explicit unsupported-path handling where semantics differ

- [x] **C3** `core/execution/kraken/kraken_connector.cpp` + `core/execution/live_connector_base.hpp` — **Kraken live connector violates current Spot REST order semantics**
  The connector omits the limit price on limit orders, maps `cancel_all()` onto `CancelAllOrdersAfter` instead of the ordinary cancel-all surface, and relies on a shared auth path that does not model Kraken's signed POST contract precisely enough. Treat Kraken live execution as unsafe until it is reworked against the current docs.
  - acceptance criteria:
    - [x] Limit-order placement includes all currently documented Kraken fields required for the chosen order type, including price for limit orders
    - [x] Private REST authentication matches Kraken's current signing model and passes golden tests built from official examples
    - [x] `cancel_all()` uses the correct Kraken endpoint/semantics instead of the dead-man-switch `CancelAllOrdersAfter` path
    - [x] Order modification explicitly distinguishes `AmendOrder` vs `EditOrder`, with tests documenting which path is used

- [ ] **C4** `core/execution/okx/okx_connector.cpp` + `core/execution/live_connector_base.hpp` — **OKX connector omits mandatory auth and trade parameters from current v5 docs**
  The connector does not send the documented OKX passphrase header, omits `tdMode` on order placement, under-specifies cancel/query/amend requests, and misuses `cancel-batch-orders` as a symbol-scoped cancel-all path. The current OKX live connector should not be used in production.
  - acceptance criteria:
    - [ ] Private REST auth includes the full documented OKX header set, including passphrase, with fixture tests proving signature correctness
    - [ ] Placement payloads include all mandatory venue fields for the supported trading mode, including `tdMode`
    - [ ] Cancel/query/amend requests include the documented identifiers such as `instId` plus `ordId`/`clOrdId` as required by the current API
    - [ ] `cancel_all()` is re-implemented using a documented OKX contract or marked unsupported with an explicit strategy-layer fallback

- [ ] **C5** `core/execution/coinbase/coinbase_connector.cpp` + `core/execution/live_connector_base.hpp` — **Coinbase connector is still speaking a legacy auth/request dialect instead of current Advanced Trade**
  The connector uses legacy `CB-ACCESS-*` style signing instead of bearer JWT authentication, submits orders with a non-current payload schema, and uses unsupported request shapes for edit/cancel-all behavior. As written, the Coinbase execution connector is not aligned with current Advanced Trade docs.
  - acceptance criteria:
    - [ ] Authentication is migrated to Coinbase's current bearer-JWT flow using the documented CDP key/private-key process
    - [ ] `submit_order` uses the current Advanced Trade create-order schema, including `client_order_id` and the documented order-configuration object for supported order types
    - [ ] Edit/cancel/query/fills/account calls are rebuilt against the current Advanced Trade REST contracts with fixture coverage from official examples
    - [ ] If Coinbase does not provide a true product-scoped cancel-all matching the internal interface, that capability is explicitly modeled as unsupported rather than emulated with an undocumented payload

---

## Reference

- Full audit report: `docs/reports/AUDIT_REPORT_2026-03-19.md`
