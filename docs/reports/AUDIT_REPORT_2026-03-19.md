# ThamesRiverTrading — Execution Connector Exchange-Doc Audit

**Audit date:** 2026-03-19  
**Scope:** `core/execution/` live order-entry and reconciliation connectors for Binance, Kraken, OKX, and Coinbase.  
**Method:** Static code audit of the current connector implementations against each exchange's current official REST/API documentation.

---

## Executive Verdict (Direct)

The live execution layer is **materially out of spec on all four exchanges**.

This is not a polish issue. It is a **production-blocking correctness issue**:
- several endpoints are called with the wrong path or wrong transport shape,
- multiple venues are signed with the wrong authentication contract,
- required request parameters are omitted,
- and at least two replace/cancel-all flows do not match the documented API semantics at all.

**Bottom line:** the current `core/execution/` connectors should be treated as **non-deployable for live trading** until they are reworked against the venue docs and validated on testnet/sandbox where available.

---

## Sources used

### Official exchange docs
- **Binance Spot API**
  - Request Security: <https://developers.binance.com/docs/binance-spot-api-docs/rest-api/request-security>
  - Trading endpoints: <https://developers.binance.com/docs/binance-spot-api-docs/rest-api/trading-endpoints>
  - Account endpoints: <https://developers.binance.com/docs/binance-spot-api-docs/rest-api/account-endpoints>
- **Kraken Spot REST API**
  - Add Order: <https://docs.kraken.com/api/docs/rest-api/add-order/>
  - Amend Order: <https://docs.kraken.com/api/docs/rest-api/amend-order/>
  - Edit Order: <https://docs.kraken.com/api/docs/rest-api/edit-order/>
  - Cancel Order / Cancel All Orders After X / Get Open Orders / Query Orders Info / Get Account Balance / Get Trades History
- **OKX API v5**
  - REST trade/account docs: <https://www.okx.com/docs-v5/en/>
- **Coinbase Advanced Trade API**
  - REST overview: <https://docs.cdp.coinbase.com/coinbase-app/advanced-trade-apis/rest-api>
  - API key auth: <https://docs.cdp.coinbase.com/coinbase-app/authentication-authorization/api-key-authentication>
  - Create / Cancel / Edit / List Orders / List Accounts / List Fills / Get Order pages under `api-reference/advanced-trade-api/rest-api`

### Repository code reviewed
- `core/execution/live_connector_base.hpp`
- `core/execution/binance/binance_connector.cpp`
- `core/execution/kraken/kraken_connector.cpp`
- `core/execution/okx/okx_connector.cpp`
- `core/execution/coinbase/coinbase_connector.cpp`

---

## Per-exchange audit findings

## 1) Binance Spot

### What the docs require
The current Binance Spot docs describe signed REST trading calls using `timestamp` and `signature`, with the documented trading surface centered on:
- `POST /api/v3/order` for new orders,
- `DELETE /api/v3/order` for cancels,
- `DELETE /api/v3/openOrders` for cancel-all on a symbol,
- `PUT /api/v3/order/amend/keepPriority` or `POST /api/v3/order/cancelReplace` for supported modification flows,
- `GET /api/v3/order`, `GET /api/v3/openOrders`, `GET /api/v3/account`, and `GET /api/v3/myTrades` for status/reconciliation.

The docs also require request fields such as:
- `symbol` for almost every trading and query call,
- `timeInForce` for limit orders,
- `timestamp` on signed endpoints,
- `symbol` on `myTrades`.

### What the implementation does
The connector currently:
- builds JSON bodies for Binance order entry,
- signs requests via custom `X-MBX-*` headers in `LiveConnectorBase`,
- omits `timeInForce` on limit orders,
- queries/cancels by `orderId` without including `symbol`,
- uses `PUT /api/v3/order?orderId=...` for replace,
- calls `GET /api/v3/myTrades` with no `symbol`.

### Gap assessment
**Severity: CRITICAL**

#### Concrete gaps
1. **Authentication/transport contract mismatch**  
   The connector signs Binance like a header-auth API. Binance Spot signed endpoints are documented around `timestamp` + `signature` request parameters, not `X-MBX-SIGNATURE`/`X-MBX-TIMESTAMP` headers.

2. **Order create payload is incomplete for limit orders**  
   `timeInForce` is not sent for limit orders, so a documented mandatory parameter is missing.

3. **Cancel/query flows omit required `symbol`**  
   The code only sends `orderId`, but Binance query/cancel endpoints require `symbol` plus one of `orderId` or `origClientOrderId`.

4. **Replace flow is not the documented Binance replace/amend API**  
   `PUT /api/v3/order?orderId=...` is not the documented Spot modification surface. Current docs expose `cancelReplace` and `amend/keepPriority` semantics instead.

5. **Trade-history reconciliation is invalid as implemented**  
   `GET /api/v3/myTrades` requires `symbol`; the current connector omits it.

### Verdict
The Binance connector is **not aligned closely enough to the current Spot API to rely on for live order entry**. Even before business-logic concerns, the wire contract itself is wrong in several places.

---

## 2) Kraken Spot REST

### What the docs require
Current Kraken Spot REST docs document:
- `POST /private/AddOrder` for order placement,
- `POST /private/AmendOrder` for in-place modification where IDs stay the same,
- `POST /private/EditOrder` as the older cancel/recreate-style edit flow,
- `POST /private/CancelOrder` and `POST /private/CancelAll`,
- `POST /private/OpenOrders`, `POST /private/QueryOrders`, `POST /private/Balance`, `POST /private/TradesHistory`.

Kraken also distinguishes order-management semantics carefully:
- limit orders need a limit price,
- amend and edit are different operations,
- `CancelAllOrdersAfter` is a dead-man-switch style control, not a symbol-scoped cancel-all surrogate.

### What the implementation does
The connector currently:
- sends `pair`, `type`, `ordertype`, and `volume`, but never includes limit `price`,
- posts to `EditOrder` for replace,
- uses `CancelAllOrdersAfter` with a `pair=` payload for `cancel_all`,
- relies on generic auth headers from `LiveConnectorBase` rather than Kraken's documented request-signing contract.

### Gap assessment
**Severity: CRITICAL**

#### Concrete gaps
1. **Limit-order placement is incomplete**  
   The connector never sends the price field for limit orders. That is a direct venue-contract violation.

2. **`cancel_all()` is mapped to the wrong endpoint and wrong semantics**  
   `CancelAllOrdersAfter` is not “cancel all open orders on symbol.” It is a timeout-based dead-man-switch control. Using it as ordinary cancel-all is incorrect.

3. **Modification path is behind the current preferred contract**  
   Kraken now documents `AmendOrder` for in-place modification where IDs stay the same. The connector still uses `EditOrder`, which has materially different semantics.

4. **Authentication likely does not match Kraken's signed POST contract**  
   The shared auth layer does not model Kraken's request-body-centric signing flow closely enough. This must be treated as a likely wire-level auth defect until proven otherwise with fixture tests against the official examples.

### Verdict
The Kraken connector is **structurally unfit for live use in its current form**. The missing limit price alone is enough to reject deployment; the cancel-all misuse makes it worse.

---

## 3) OKX API v5

### What the docs require
Current OKX v5 trade/account docs document:
- `POST /api/v5/trade/order` for placement,
- `POST /api/v5/trade/cancel-order` for cancel,
- `POST /api/v5/trade/amend-order` for modify,
- `GET /api/v5/trade/orders-pending`,
- `GET /api/v5/account/balance`,
- `GET /api/v5/account/positions`,
- `GET /api/v5/trade/fills` / related fills history endpoints.

OKX signed private REST calls require the OKX auth header set, including:
- `OK-ACCESS-KEY`,
- `OK-ACCESS-SIGN`,
- `OK-ACCESS-TIMESTAMP`,
- `OK-ACCESS-PASSPHRASE`.

Order endpoints also require exchange-specific fields such as `tdMode`, and order-management requests generally key off `instId` plus `ordId`/`clOrdId`.

### What the implementation does
The connector currently:
- omits `tdMode` on placement,
- signs with key/sign/timestamp but not passphrase,
- cancels/amends/queries using only `ordId`,
- implements `cancel_all()` by posting `{"instId": ...}` to `/api/v5/trade/cancel-batch-orders`.

### Gap assessment
**Severity: CRITICAL**

#### Concrete gaps
1. **Authentication header set is incomplete**  
   The shared auth layer does not send `OK-ACCESS-PASSPHRASE`, which is part of the documented private REST auth contract.

2. **Placement payload is missing mandatory venue fields**  
   `tdMode` is absent, so even the basic order-entry payload is incomplete versus the current docs.

3. **Cancel/query/amend payloads are underspecified**  
   OKX order-management calls are documented around `instId` plus `ordId`/`clOrdId`; the connector only sends `ordId`.

4. **Cancel-all implementation does not match the endpoint contract**  
   `cancel-batch-orders` expects an order list payload, not a bare `instId`. The current `cancel_all()` implementation is not aligned with the documented endpoint semantics.

### Verdict
The OKX connector is **not exchange-correct yet**. The auth omission and wrong cancel-all shape are both live-blocking.

---

## 4) Coinbase Advanced Trade

### What the docs require
Current Coinbase Advanced Trade docs use:
- `Authorization: Bearer <JWT>` authentication derived from CDP API keys/private keys,
- the modern `POST /api/v3/brokerage/orders` create-order contract with `client_order_id` and a structured `order_configuration`,
- documented cancel/edit/list/get/fills/account endpoints under the Advanced Trade REST surface.

This is **not** the old Coinbase Exchange HMAC header model.

### What the implementation does
The connector currently:
- signs Coinbase requests with `CB-ACCESS-KEY`, `CB-ACCESS-TIMESTAMP`, and `CB-ACCESS-SIGN`,
- builds a flat payload containing `product_id`, `side`, `order_type`, and `size`,
- edits orders with a simplified payload that does not follow the documented edit-order schema,
- implements `cancel_all()` by sending `{"product_id": ...}` to the batch cancel endpoint.

### Gap assessment
**Severity: CRITICAL**

#### Concrete gaps
1. **Authentication contract is wrong**  
   Advanced Trade uses bearer JWT auth. The connector still uses a legacy HMAC-style header set.

2. **Create-order payload is not the documented Advanced Trade schema**  
   The current payload shape does not follow the documented `client_order_id` + `order_configuration` request model.

3. **Edit-order flow is not aligned with the current API contract**  
   The simplified edit payload does not match Coinbase's documented edit-order request structure.

4. **`cancel_all()` uses an unsupported request shape**  
   The batch cancel endpoint is documented around explicit `order_ids`; posting only a `product_id` is not a documented substitute for venue-native cancel-all.

### Verdict
The Coinbase connector is **effectively speaking the wrong API dialect** for current Advanced Trade.

---

## Cross-connector systemic issues

## 1) Shared auth abstraction is too generic for venue-specific reality
`LiveConnectorBase::auth_headers()` tries to normalize four materially different exchange auth schemes into one common helper. That abstraction is currently too shallow.

Result: venue-specific mandatory fields are silently lost:
- Binance signed params,
- Kraken nonce/body signing behavior,
- OKX passphrase,
- Coinbase JWT bearer auth.

**Assessment:** the shared auth abstraction is currently a source of defects, not safety.

## 2) Replace semantics are incorrectly normalized
The code assumes every venue has a clean “replace order” primitive with similar semantics. The docs do not support that assumption.
- Binance: amend-keep-priority and cancel-replace are distinct operations.
- Kraken: amend vs edit have different identity/queue implications.
- OKX: amend order has venue-specific field requirements.
- Coinbase: edit contract is Advanced-Trade-specific.

**Assessment:** `replace_order()` needs venue-specific capability modeling rather than one generic happy-path abstraction.

## 3) Reconciliation paths are under-specified by symbol/product requirements
Several reconciliation calls ignore symbol/product requirements in the current venue docs, especially on Binance and likely other venues where fills/open-order/history queries are scoped.

**Assessment:** reconciliation correctness is currently weaker than the code implies.

---

## Industry-standard comparison

For a serious high/mid-frequency execution stack, exchange connectors are expected to have:
1. **Spec-accurate request/response fixtures per endpoint**,
2. **Venue-native auth/signing tests against official examples**,
3. **Capability-mapped order semantics** instead of pretending all venues support the same modify/cancel-all abstractions,
4. **Testnet/sandbox certification** before promotion,
5. **Explicit unsupported-feature handling** where a venue contract does not match the internal interface.

This repository is **below that standard today** in the execution layer.

---

## Required remediation plan

## Immediate (before any live deployment)
1. Rebuild each connector directly from the current official docs.
2. Split auth/signing into venue-native implementations instead of one generic helper.
3. Rework `replace_order()` into a capability-aware interface:
   - native amend,
   - cancel-replace,
   - unsupported.
4. Rework `cancel_all()` per venue instead of forcing one semantic across mismatched APIs.
5. Add golden request/response fixtures for submit/cancel/amend/query/open-orders/fills/balance.

## Next validation layer
6. Add testnet/sandbox conformance tests for every supported exchange.
7. Add a startup self-check that verifies credentials and required capability availability per venue.
8. Log the exact venue endpoint, required params, and rejection reason on every REST failure.

---

## Final verdict

If you turned these connectors loose with real money today, you would be depending on an execution layer that is **materially out of contract with Binance, Kraken, OKX, and Coinbase**.

That is a hard stop.

The feed side may be improving, but the live execution side still needs a **full exchange-doc conformance rewrite** before the stack can make a credible claim of live readiness.
