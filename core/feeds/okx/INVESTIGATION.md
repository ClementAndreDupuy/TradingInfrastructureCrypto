# OKX Feed Handler — Connection Failure Investigation

**Date:** 2026-03-18
**Branch:** `claude/investigate-okx-feed-handler-qqcUT`
**Files analysed:** `okx/okx_feed_handler.cpp`, compared against `binance/` and `kraken/`

---

## tl;dr

The OKX handler fails to connect (i.e. `start()` times out and returns
`ERROR_CONNECTION_LOST`) because of **two compounding bugs**:

1. **`validate_checksum` reconstructs the price side of the CRC32 string from
   `double` keys, producing the wrong string representation.**
   This makes every single delta checksum check fail, which fires
   `trigger_resnapshot` on the very first streaming message, putting the
   handler back into `BUFFERING` and preventing it from ever reaching
   `STREAMING`.

2. **Delta buffering is non-functional**: `process_message` (which fills
   `delta_buffer_`) is called exclusively from within `lws_service`, but
   `lws_service` is never called during the `BUFFERING` window (between WS
   establishment and the start of the streaming loop).
   `apply_buffered_deltas` therefore always runs on an empty buffer; the first
   real messages arrive with `state_ == STREAMING`, hitting the broken checksum
   path immediately.

---

## Bug 1 (Critical) — Checksum price round-trip loses precision / format

### Where
`okx_feed_handler.cpp` → `validate_checksum()`, lines 592–611

### What the code does
The per-delta CRC32 verification is computed by serialising the top-25 bid and
ask price levels into a colon-separated string. Prices are stored in two
`std::map<double, std::string>` containers (`bids_` / `asks_`), where the key
is the **double** representation of the original price string.

When building the checksum string the code does:
```cpp
oss << std::setprecision(15) << bid_it->first << ':' << bid_it->second;
```
`bid_it->first` is a `double`; the quantity (`bid_it->second`) is the original
string, but the **price** has been through `std::stod` and back.

### Why it breaks
OKX's CRC32 requires the **exact wire strings** the exchange sent.  For most
prices this round-trip looks fine, but there are common failure modes:

| Wire price | Stored double | `setprecision(15)` output | CRC input expected |
|---|---|---|---|
| `"95000.10"` | 95000.1 | `"95000.1"` | `"95000.10"` |
| `"0.00001234"` | 1.234e-05 | `"1.234e-05"` | `"0.00001234"` |
| `"12345.678901234567"` | (truncated) | different digits | original string |

The trailing-zero and scientific-notation mismatches cause virtually every
checksum to fail in practice.

### Contrast with Binance / Kraken
Neither Binance nor Kraken verifies a CRC checksum on incremental deltas at
all. Binance stores no local book mirror; Kraken uses its own checksum scheme
from snapshot data. The checksum path is OKX-specific, so this bug only
surfaces here.

### Fix needed
Store prices as their original string in the book map (or in a parallel
structure) and use those strings when building the CRC input, instead of
formatting the double key back to a string.

---

## Bug 2 (Significant) — Delta buffering window never receives messages

### Where
`okx_feed_handler.cpp` → `ws_event_loop()`, lines 242–268

### What happens (correct intention)
The design intention is:
1. Connect WS → enter `BUFFERING` state.
2. While WS buffers incoming deltas in `delta_buffer_`, fetch REST snapshot.
3. Replay buffered deltas against the snapshot.
4. Enter `STREAMING`.

### Why it is broken
`process_message` (which pushes messages onto `delta_buffer_`) is only ever
called from inside `LWS_CALLBACK_CLIENT_RECEIVE`, which fires only when
`lws_service` is called.

After the connection establishment loop exits:
```cpp
while (running_.load() && !session.established && !session.closed)
    lws_service(ctx, 50);   // ← last call to lws_service before snapshot
```
…the code goes straight into the blocking REST call `fetch_snapshot()` and
then `apply_buffered_deltas()`, **without ever calling `lws_service` again**.

WS messages that arrive from OKX during this period are queued inside
libwebsockets' receive buffer, but `LWS_CALLBACK_CLIENT_RECEIVE` never fires,
so `delta_buffer_` stays empty.

`apply_buffered_deltas` therefore processes zero messages and returns
`SUCCESS`, the state is promoted to `STREAMING`, and the very first
`lws_service` call in the streaming loop delivers all queued messages at once,
each of which hits the broken checksum check and triggers an infinite
resnapshot loop.

### Contrast with Binance / Kraken
Binance and Kraken have exactly the same structural issue (they also pause
`lws_service` during snapshot fetch), but they work because:

- **Binance** uses a first-update-id window rule (delta `U ≤ last_snapshot+1 ≤ u`)
  that gracefully tolerates buffered deltas arriving just after the snapshot ID.
- **Kraken** uses strictly-sequential `seq` numbers but has no CRC check.

Neither relies on a correctly-populated `delta_buffer_` to avoid a
connection failure, because their sequence and checksum logic can absorb a
brief gap. OKX's CRC requirement makes the empty-buffer path fatal.

---

## Bug 3 (Minor) — Missing SYNCHRONIZED intermediate state

Both Binance and Kraken use four states:
```
DISCONNECTED → BUFFERING → SYNCHRONIZED → STREAMING
```
The `SYNCHRONIZED` state means: snapshot applied, now replaying buffered
deltas. Any new messages that arrive during replay are processed inline
(validated and applied immediately) rather than buffered again.

OKX has only three states (`DISCONNECTED / BUFFERING / STREAMING`), skipping
`SYNCHRONIZED`. In practice this is not observable given Bug 2, but it means
`apply_buffered_deltas` cannot safely interleave with new incoming messages on
reconnect.

---

## Bug 4 (Minor) — Streaming loop polls at 50ms; Binance / Kraken use 1000ms

```cpp
// OKX streaming loop
lws_service(ctx, 50);   // polls every 50ms

// Binance / Kraken streaming loops
lws_service(ctx, 1000); // polls every 1s (latency handled by cancel_service)
```
The 50ms poll wastes CPU but does not break connectivity.

---

## Connection failure sequence (end-to-end)

```
start()
  └─ sets state = BUFFERING
  └─ spawns ws_thread_
  └─ waits up to 30 s for state == STREAMING

ws_event_loop()
  └─ lws_create_context + lws_client_connect_via_info  → OK
  └─ waits for session.established                     → OK
  └─ state = BUFFERING, delta_buffer_.clear()
  └─ fetch_snapshot() → REST GET /api/v5/market/books  → OK, last_sequence_ set
  └─ apply_buffered_deltas()                           → delta_buffer_ empty, returns SUCCESS
  └─ state = STREAMING, ws_cv_.notify_all()

  streaming loop (lws_service called for the first time since WS established)
  └─ all queued WS deltas arrive at once in STREAMING state
  └─ validate_delta_sequence()    → passes (prevSeqId matches snapshot seqId)
  └─ validate_checksum()          → FAILS (price double → string mismatch)
  └─ trigger_resnapshot()         → state = BUFFERING
  └─ start() receives notify but state is BUFFERING again → wait continues...
  └─ 30 s timeout expires → start() returns ERROR_CONNECTION_LOST
```

---

## Recommended fixes (priority order)

1. **Fix `validate_checksum`**: store price as original wire string alongside
   the double key (e.g. `std::map<double, std::pair<std::string, std::string>>`
   or a separate `std::map<double, std::string>` for prices) and use the
   string directly when serialising the CRC input.

2. **Fix delta buffering**: call `lws_service` on a background poller (or use
   a non-blocking approach) while `fetch_snapshot` is running, so deltas are
   genuinely buffered before the snapshot sequence anchor is established.
   Alternatively, adopt Binance's window-based approach where deltas are
   accepted as long as they span the snapshot's `lastUpdateId`.

3. **Add SYNCHRONIZED state**: mirror Binance/Kraken so that messages arriving
   during `apply_buffered_deltas` are processed inline rather than discarded or
   double-buffered.
