import asyncio
import math
import time
from collections import defaultdict
from datetime import datetime, timezone

import structlog

from config import Settings
from market_maker import SpreadDetector
from models import (
    BookUpdate,
    CombinatorialSignal,
    Leg,
    NegRiskSignal,
    ParitySignal,
    Signal,
    ValidatedSignal,
    WhaleSignal,
)

log = structlog.get_logger()

DEBOUNCE_SECS = 0.5


def _kelly_size(net_profit_bps: float, available_size: float, config: Settings) -> float:
    edge = net_profit_bps / 10_000
    loss = config.kelly_arb_loss_bps / 10_000
    p = config.kelly_p_win
    if edge <= 0 or loss <= 0:
        return 0.0
    f_star = max(0.0, p / loss - (1.0 - p) / edge)
    return min(f_star * config.max_exposure * config.kelly_fraction, available_size)


def _expiry_ts(end_date: str) -> float:
    if not end_date:
        return math.inf
    try:
        dt = datetime.fromisoformat(end_date.replace("Z", "+00:00"))
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        return dt.timestamp()
    except (ValueError, TypeError):
        return math.inf


class NegRiskDetector:
    def __init__(self, groups: dict, books: dict, config: Settings):
        self._groups = groups
        self._books = books
        self._config = config
        self._token_to_group: dict[str, str] = {}
        self._token_to_condition: dict[str, str] = {}
        self._group_expiry_ts: dict[str, float] = {}
        self._group_yes_tokens: dict[str, list[str]] = {}
        self._last_signal_ts: dict[str, float] = {}

    def rebuild_index(self) -> None:
        self._token_to_group = {
            tid: gid
            for gid, markets in self._groups.items()
            for m in markets
            for tid in m.token_ids
        }
        self._token_to_condition = {
            tid: m.condition_id
            for markets in self._groups.values()
            for m in markets
            for tid in m.token_ids
        }
        self._group_expiry_ts = {
            gid: _expiry_ts(markets[0].end_date)
            for gid, markets in self._groups.items()
            if markets
        }
        new_yes_tokens: dict[str, list[str]] = {}
        skipped_groups: list[str] = []
        for gid, markets in self._groups.items():
            if not markets:
                continue
            yes_tokens = [m.yes_token_id for m in markets]
            if all(yes_tokens):
                new_yes_tokens[gid] = yes_tokens
            else:
                skipped_groups.append(gid)
        self._group_yes_tokens = new_yes_tokens
        if skipped_groups:
            log.info(
                "negrisk_index_skipped",
                reason="incomplete_yes_coverage",
                count=len(skipped_groups),
                group_ids=skipped_groups,
            )

    def on_update(self, update: BookUpdate) -> NegRiskSignal | None:
        group_id = self._token_to_group.get(update.token_id)
        if group_id is None:
            return None

        expiry_ts = self._group_expiry_ts.get(group_id, math.inf)
        time_to_expiry_secs = expiry_ts - time.time()
        if time_to_expiry_secs < self._config.min_time_to_expiry_secs:
            return None

        yes_token_ids = self._group_yes_tokens.get(group_id)
        if not yes_token_ids:
            return None

        if not all(tid in self._books for tid in yes_token_ids):
            return None

        now = time.monotonic()
        fee = self._config.fee_estimate_bps / 10_000

        long_key = f"{group_id}:LONG"
        if (
                self._config.enable_negrisk_long
                and now - self._last_signal_ts.get(long_key, 0.0) >= DEBOUNCE_SECS
        ):
            estimates = [
                self._books[tid].estimate_asks(self._config.max_exposure)
                for tid in yes_token_ids
            ]
            if all(est.available_size > 0 for est in estimates):
                sum_vwap = sum(est.vwap_price for est in estimates)
                long_profit = 1.0 - sum_vwap - fee
                if long_profit > self._config.min_profit_bps_long / 10_000:
                    legs = [
                        Leg(tid, self._token_to_condition[tid], est.vwap_price, est.available_size, neg_risk=True, side="BUY")
                        for tid, est in zip(yes_token_ids, estimates)
                    ]
                    size = _kelly_size(long_profit * 10_000, min(l.available_size for l in legs), self._config)
                    if size < self._config.min_order_size:
                        self._last_signal_ts[long_key] = now
                        log.info(
                            "signal_rejected",
                            strategy="negrisk_long",
                            group_id=group_id,
                            reason="size_too_small",
                        )
                        return None
                    self._last_signal_ts[long_key] = now
                    log.info(
                        "signal_generated",
                        strategy="negrisk_long",
                        group_id=group_id,
                        net_profit_bps=round(long_profit * 10_000, 2),
                        size=size,
                        direction="LONG",
                        time_to_expiry_secs=round(time_to_expiry_secs),
                    )
                    return NegRiskSignal("LONG", group_id, legs, size, 1.0 - sum_vwap, long_profit, now,
                                         time_to_expiry_secs)

        short_key = f"{group_id}:SHORT"
        if (
                self._config.enable_negrisk_short
                and now - self._last_signal_ts.get(short_key, 0.0) >= DEBOUNCE_SECS
        ):
            estimates = [
                self._books[tid].estimate_bids(self._config.max_exposure)
                for tid in yes_token_ids
            ]
            if all(est.available_size > 0 for est in estimates):
                sum_vwap = sum(est.vwap_price for est in estimates)
                short_profit = sum_vwap - 1.0 - fee
                if short_profit > self._config.min_profit_bps_short / 10_000:
                    legs = [
                        Leg(tid, self._token_to_condition[tid], est.vwap_price, est.available_size, neg_risk=True, side="SELL")
                        for tid, est in zip(yes_token_ids, estimates)
                    ]
                    size = _kelly_size(short_profit * 10_000, min(l.available_size for l in legs), self._config)
                    if size < self._config.min_order_size:
                        self._last_signal_ts[short_key] = now
                        log.info(
                            "signal_rejected",
                            strategy="negrisk_short",
                            group_id=group_id,
                            reason="size_too_small",
                        )
                        return None
                    self._last_signal_ts[short_key] = now
                    log.info(
                        "signal_generated",
                        strategy="negrisk_short",
                        group_id=group_id,
                        net_profit_bps=round(short_profit * 10_000, 2),
                        size=size,
                        direction="SHORT",
                        time_to_expiry_secs=round(time_to_expiry_secs),
                    )
                    return NegRiskSignal("SHORT", group_id, legs, size, sum_vwap - 1.0, short_profit, now,
                                         time_to_expiry_secs)

        return None


class CombinatorialDetector:
    def __init__(self, dep_graph: list, books: dict, config: Settings):
        self._books = books
        self._config = config
        self._dep_graph: list = []
        self._token_index: dict[str, list] = defaultdict(list)
        self._last_signal_ts: dict[str, float] = {}
        self.replace_dep_graph(dep_graph)

    def replace_dep_graph(self, dep_graph: list) -> None:
        new_index: dict[str, list] = defaultdict(list)
        for dep in dep_graph:
            for tid in (
                    dep.market_a_yes_token,
                    dep.market_a_no_token,
                    dep.market_b_yes_token,
                    dep.market_b_no_token,
            ):
                if tid:
                    new_index[tid].append(dep)
        self._dep_graph = list(dep_graph)
        self._token_index = new_index

    def dep_tokens(self) -> set[str]:
        return set(self._token_index.keys())

    @property
    def dep_count(self) -> int:
        return len(self._dep_graph)

    def on_update(self, update: BookUpdate) -> CombinatorialSignal | None:
        for dep in self._token_index.get(update.token_id, []):
            sig = self._evaluate(dep)
            if sig:
                return sig
        return None

    def _arb_sides(self, dep) -> list[tuple]:
        ctype = dep.constraint_type
        sides: list[tuple] = []
        if ctype in ("implication", "subset"):
            if dep.direction in ("A_implies_B", "bidirectional"):
                sides.append((
                    dep.market_a_no_token, dep.market_a_condition_id, dep.market_a_neg_risk,
                    dep.market_b_yes_token, dep.market_b_condition_id, dep.market_b_neg_risk,
                ))
            if dep.direction in ("B_implies_A", "bidirectional"):
                sides.append((
                    dep.market_b_no_token, dep.market_b_condition_id, dep.market_b_neg_risk,
                    dep.market_a_yes_token, dep.market_a_condition_id, dep.market_a_neg_risk,
                ))
        elif ctype == "mutual_excl":
            sides.append((
                dep.market_a_no_token, dep.market_a_condition_id, dep.market_a_neg_risk,
                dep.market_b_no_token, dep.market_b_condition_id, dep.market_b_neg_risk,
            ))
        return sides

    def _eval_pair(
            self, ta: str, ca: str, na: bool, tb: str, cb: str, nb: bool,
    ) -> tuple[CombinatorialSignal | None, bool]:
        if not ta or not tb:
            return None, False

        book_a = self._books.get(ta)
        book_b = self._books.get(tb)
        if book_a is None or book_b is None:
            return None, False

        now = time.monotonic()
        max_age = self._config.max_comb_book_age_ms / 1000
        if now - book_a.last_update_ts > max_age or now - book_b.last_update_ts > max_age:
            return None, False

        est_a = book_a.estimate_asks(self._config.max_exposure)
        est_b = book_b.estimate_asks(self._config.max_exposure)
        if est_a.available_size <= 0 or est_b.available_size <= 0:
            return None, False
        if (
                est_a.available_size < self._config.min_comb_liquidity
                or est_b.available_size < self._config.min_comb_liquidity
        ):
            return None, False

        fee = self._config.fee_estimate_bps / 10_000
        min_edge = self._config.min_profit_bps_comb / 10_000

        p_a = est_a.vwap_price
        p_b = est_b.vwap_price
        cost = p_a + p_b
        gross = 1.0 - cost
        net = gross - fee
        if net <= min_edge:
            return None, False

        size = _kelly_size(net * 10_000, min(est_a.available_size, est_b.available_size), self._config)
        if size < self._config.min_order_size:
            return None, True

        legs = [
            Leg(ta, ca, p_a, est_a.available_size, neg_risk=na, side="BUY"),
            Leg(tb, cb, p_b, est_b.available_size, neg_risk=nb, side="BUY"),
        ]
        sig = CombinatorialSignal(
            "", ta, tb, "", "", legs, size, gross, net, now, math.inf,
        )
        return sig, False

    def _evaluate(self, dep) -> CombinatorialSignal | None:
        sides = self._arb_sides(dep)
        if not sides:
            return None

        expiry_a = _expiry_ts(dep.market_a_end_date)
        expiry_b = _expiry_ts(dep.market_b_end_date)
        tte = min(expiry_a, expiry_b) - time.time()
        if not math.isfinite(tte) or tte < self._config.min_time_to_expiry_secs:
            return None

        now = time.monotonic()
        if now - self._last_signal_ts.get(dep.dep_id, 0.0) < DEBOUNCE_SECS:
            return None

        candidates: list[CombinatorialSignal] = []
        had_size_too_small = False
        for ta, ca, na, tb, cb, nb in sides:
            sig, size_too_small = self._eval_pair(ta, ca, na, tb, cb, nb)
            if sig is not None:
                candidates.append(sig)
            if size_too_small:
                had_size_too_small = True

        if not candidates:
            if had_size_too_small:
                self._last_signal_ts[dep.dep_id] = now
                log.info(
                    "signal_rejected",
                    strategy="combinatorial",
                    dep_id=dep.dep_id,
                    reason="size_too_small",
                )
            return None

        best = max(candidates, key=lambda s: s.net_profit)
        self._last_signal_ts[dep.dep_id] = now
        log.info(
            "signal_generated",
            strategy="combinatorial",
            dep_id=dep.dep_id,
            constraint_desc=dep.constraint_desc,
            net_profit_bps=round(best.net_profit * 10_000, 2),
            size=best.size,
            direction=dep.direction,
            time_to_expiry_secs=round(tte),
        )
        return CombinatorialSignal(
            dep.dep_id,
            best.market_a_token,
            best.market_b_token,
            dep.constraint_type,
            dep.direction,
            best.legs,
            best.size,
            best.gross_profit,
            best.net_profit,
            best.ts,
            tte,
        )


class ParityDetector:
    def __init__(self, binary_markets: dict, books: dict, config: Settings):
        self._binary_markets = binary_markets
        self._books = books
        self._config = config
        self._token_to_condition: dict[str, str] = {}
        self._condition_expiry_ts: dict[str, float] = {}
        self._last_signal_ts: dict[str, float] = {}

    def rebuild_index(self) -> None:
        self._token_to_condition = {}
        self._condition_expiry_ts = {}
        for cid, m in self._binary_markets.items():
            for tid in m.token_ids:
                self._token_to_condition[tid] = cid
            self._condition_expiry_ts[cid] = _expiry_ts(m.end_date)

    def binary_tokens(self) -> set[str]:
        return set(self._token_to_condition.keys())

    def on_update(self, update: BookUpdate) -> ParitySignal | None:
        condition_id = self._token_to_condition.get(update.token_id)
        if condition_id is None:
            return None

        market = self._binary_markets.get(condition_id)
        if market is None or not market.yes_token_id or not market.no_token_id:
            return None

        expiry_ts = self._condition_expiry_ts.get(condition_id, math.inf)
        time_to_expiry_secs = expiry_ts - time.time()
        if time_to_expiry_secs < self._config.min_time_to_expiry_secs:
            return None

        yes_tid = market.yes_token_id
        no_tid = market.no_token_id
        if yes_tid not in self._books or no_tid not in self._books:
            return None

        now = time.monotonic()
        if now - self._last_signal_ts.get(condition_id, 0.0) < DEBOUNCE_SECS:
            return None

        est_yes = self._books[yes_tid].estimate_asks(self._config.max_exposure)
        est_no = self._books[no_tid].estimate_asks(self._config.max_exposure)
        if est_yes.available_size <= 0 or est_no.available_size <= 0:
            return None

        fee = self._config.fee_estimate_bps / 10_000
        sum_vwap = est_yes.vwap_price + est_no.vwap_price
        gross = 1.0 - sum_vwap
        net = gross - fee

        if net <= self._config.min_profit_bps_parity / 10_000:
            return None

        legs = [
            Leg(yes_tid, condition_id, est_yes.vwap_price, est_yes.available_size, neg_risk=False, side="BUY"),
            Leg(no_tid, condition_id, est_no.vwap_price, est_no.available_size, neg_risk=False, side="BUY"),
        ]
        size = _kelly_size(net * 10_000, min(l.available_size for l in legs), self._config)

        if size < self._config.min_order_size:
            self._last_signal_ts[condition_id] = now
            log.info("signal_rejected", strategy="parity_arb", condition_id=condition_id, reason="size_too_small")
            return None

        self._last_signal_ts[condition_id] = now
        log.info(
            "signal_generated",
            strategy="parity_arb",
            condition_id=condition_id,
            net_profit_bps=round(net * 10_000, 2),
            size=size,
            time_to_expiry_secs=round(time_to_expiry_secs),
        )
        return ParitySignal(condition_id, legs, size, gross, net, now, time_to_expiry_secs)


class DetectorPipeline:
    def __init__(
            self,
            groups: dict,
            dep_graph: list,
            books: dict,
            config: Settings,
            binary_markets: dict | None = None,
            spread_queue: asyncio.Queue | None = None,
    ):
        self._config = config
        self._spread_queue = spread_queue
        _binary = binary_markets if binary_markets is not None else {}
        self._negrisk = NegRiskDetector(groups, books, config)
        self._comb = CombinatorialDetector(dep_graph, books, config)
        self._parity = ParityDetector(_binary, books, config)
        self._spread = SpreadDetector(groups, _binary, books, config)

    def rebuild_index(self) -> None:
        self._negrisk.rebuild_index()
        self._spread.rebuild_index()

    def rebuild_parity_index(self) -> None:
        self._parity.rebuild_index()
        self._spread.rebuild_index()

    def parity_tokens(self) -> set[str]:
        return self._parity.binary_tokens()

    def replace_dep_graph(self, dep_graph: list) -> None:
        self._comb.replace_dep_graph(dep_graph)

    def dep_tokens(self) -> set[str]:
        return self._comb.dep_tokens()

    @property
    def dep_count(self) -> int:
        return self._comb.dep_count

    def on_update(self, update: BookUpdate) -> Signal | None:
        if self._config.enable_negrisk_long or self._config.enable_negrisk_short:
            sig = self._negrisk.on_update(update)
            if sig:
                return sig

        if self._config.enable_combinatorial:
            sig = self._comb.on_update(update)
            if sig:
                return sig

        if self._config.enable_parity_arb:
            sig = self._parity.on_update(update)
            if sig:
                return sig

        if self._config.enable_spread_farming and self._spread_queue is not None:
            spread_sig = self._spread.on_update(update)
            if spread_sig is not None:
                self._spread_queue.put_nowait(spread_sig)

        return None


class WhaleDetector:
    def __init__(self, config: Settings):
        self._config = config

    def validate_signal(self, signal: WhaleSignal, books: dict) -> ValidatedSignal | None:
        return _validate_whale(signal, books, self._config)


def validate(signal: Signal, books: dict, config: Settings) -> ValidatedSignal | None:
    if isinstance(signal, NegRiskSignal):
        return _validate_negrisk(signal, books, config)
    if isinstance(signal, CombinatorialSignal):
        return _validate_combinatorial(signal, books, config)
    if isinstance(signal, ParitySignal):
        return _validate_parity(signal, books, config)
    if isinstance(signal, WhaleSignal):
        return _validate_whale(signal, books, config)
    return None


def _validate_negrisk(signal: NegRiskSignal, books: dict, config: Settings) -> ValidatedSignal | None:
    side = "BUY" if signal.direction == "LONG" else "SELL"
    legs = []
    for leg in signal.legs:
        book = books.get(leg.token_id)
        if book is None:
            log.info("signal_rejected", strategy="negrisk_long" if signal.direction == "LONG" else "negrisk_short",
                     group_id=signal.group_id, reason="no_liquidity")
            return None
        if signal.direction == "LONG":
            price = book.best_ask()
            if price is None:
                log.info("signal_rejected", strategy="negrisk_long", group_id=signal.group_id, reason="no_liquidity")
                return None
            est = book.estimate_asks(config.max_exposure)
        else:
            price = book.best_bid()
            if price is None:
                log.info("signal_rejected", strategy="negrisk_short", group_id=signal.group_id, reason="no_liquidity")
                return None
            est = book.estimate_bids(config.max_exposure)
        legs.append(Leg(leg.token_id, leg.condition_id, price, est.available_size, neg_risk=True, side=side))

    prices = [l.price for l in legs]
    gross = (1.0 - sum(prices)) if signal.direction == "LONG" else (sum(prices) - 1.0)
    net = gross - config.fee_estimate_bps / 10_000
    threshold = config.min_profit_bps_long if signal.direction == "LONG" else config.min_profit_bps_short
    strategy = "negrisk_long" if signal.direction == "LONG" else "negrisk_short"

    if net <= threshold / 10_000:
        log.info("signal_rejected", strategy=strategy, group_id=signal.group_id, reason="price_moved")
        return None

    available = min(l.available_size for l in legs)
    size = _kelly_size(net * 10_000, available, config)
    if size < config.min_order_size:
        log.info("signal_rejected", strategy=strategy, group_id=signal.group_id, reason="size_too_small")
        return None

    return ValidatedSignal(signal, strategy, legs, size, net)


def _validate_combinatorial(
        signal: CombinatorialSignal, books: dict, config: Settings
) -> ValidatedSignal | None:
    legs = []
    for leg in signal.legs:
        book = books.get(leg.token_id)
        if book is None:
            log.info("signal_rejected", strategy="combinatorial", group_id=signal.dep_id, reason="no_liquidity")
            return None
        if time.monotonic() - book.last_update_ts > config.max_comb_book_age_ms / 1000:
            log.info("signal_rejected", strategy="combinatorial", group_id=signal.dep_id, reason="stale_book")
            return None
        ask = book.best_ask()
        if ask is None:
            log.info("signal_rejected", strategy="combinatorial", group_id=signal.dep_id, reason="no_liquidity")
            return None
        est = book.estimate_asks(config.max_exposure)
        if est.available_size < config.min_comb_liquidity:
            log.info("signal_rejected", strategy="combinatorial", group_id=signal.dep_id, reason="no_liquidity")
            return None
        legs.append(
            Leg(
                leg.token_id,
                leg.condition_id,
                ask,
                est.available_size,
                neg_risk=leg.neg_risk,
                side="BUY",
            )
        )

    if len(legs) != 2:
        log.info("signal_rejected", strategy="combinatorial", group_id=signal.dep_id, reason="no_liquidity")
        return None

    cost = sum(l.price for l in legs)
    gross = 1.0 - cost
    net = gross - config.fee_estimate_bps / 10_000

    if net <= config.min_profit_bps_comb / 10_000:
        log.info("signal_rejected", strategy="combinatorial", group_id=signal.dep_id, reason="price_moved")
        return None

    available = min(l.available_size for l in legs)
    size = _kelly_size(net * 10_000, available, config)
    if size < config.min_order_size:
        log.info("signal_rejected", strategy="combinatorial", group_id=signal.dep_id, reason="size_too_small")
        return None

    return ValidatedSignal(signal, "combinatorial", legs, size, net)


def _validate_parity(signal: ParitySignal, books: dict, config: Settings) -> ValidatedSignal | None:
    legs = []
    for leg in signal.legs:
        book = books.get(leg.token_id)
        if book is None:
            log.info("signal_rejected", strategy="parity_arb", condition_id=signal.condition_id, reason="no_liquidity")
            return None
        ask = book.best_ask()
        if ask is None:
            log.info("signal_rejected", strategy="parity_arb", condition_id=signal.condition_id, reason="no_liquidity")
            return None
        est = book.estimate_asks(config.max_exposure)
        legs.append(Leg(leg.token_id, leg.condition_id, ask, est.available_size, neg_risk=False, side="BUY"))

    if len(legs) != 2:
        log.info("signal_rejected", strategy="parity_arb", condition_id=signal.condition_id, reason="no_liquidity")
        return None

    cost = sum(l.price for l in legs)
    gross = 1.0 - cost
    net = gross - config.fee_estimate_bps / 10_000

    if net <= config.min_profit_bps_parity / 10_000:
        log.info("signal_rejected", strategy="parity_arb", condition_id=signal.condition_id, reason="price_moved")
        return None

    available = min(l.available_size for l in legs)
    size = _kelly_size(net * 10_000, available, config)
    if size < config.min_order_size:
        log.info("signal_rejected", strategy="parity_arb", condition_id=signal.condition_id, reason="size_too_small")
        return None

    return ValidatedSignal(signal, "parity_arb", legs, size, net)


def _validate_whale(signal: WhaleSignal, books: dict, config: Settings) -> ValidatedSignal | None:
    book = books.get(signal.token_id)
    if book is None:
        log.info("signal_rejected", strategy="whale_tracking", token_id=signal.token_id, reason="no_liquidity")
        return None

    if signal.side == "BUY":
        price = book.best_ask()
        if price is None:
            log.info("signal_rejected", strategy="whale_tracking", token_id=signal.token_id, reason="no_liquidity")
            return None
        est = book.estimate_asks(signal.mirror_size)
    else:
        price = book.best_bid()
        if price is None:
            log.info("signal_rejected", strategy="whale_tracking", token_id=signal.token_id, reason="no_liquidity")
            return None
        est = book.estimate_bids(signal.mirror_size)

    size = signal.mirror_size
    if size < config.min_order_size:
        log.info("signal_rejected", strategy="whale_tracking", token_id=signal.token_id, reason="size_too_small")
        return None

    leg = Leg(
        signal.token_id,
        signal.condition_id,
        price,
        est.available_size,
        neg_risk=signal.neg_risk,
        side=signal.side,
    )
    return ValidatedSignal(signal, "whale_tracking", [leg], size, config.min_profit_bps_whale / 10_000)
