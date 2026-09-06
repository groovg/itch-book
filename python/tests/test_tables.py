import datetime as dt
import math
import random

import numpy as np
import pytest

import itch_book as ib
from itch_book._dates import midnight_ns

from itch_stream import (
    add_order,
    add_order_mpid,
    broken_trade,
    cross_trade,
    luld_collar,
    noii,
    operational_halt,
    order_cancel,
    order_delete,
    order_executed,
    order_executed_price,
    order_replace,
    reg_sho,
    stock_directory,
    system_event,
    trade,
    trading_action,
)

NS = 1_000_000_000
T0 = 34_200 * NS
NAME = "12302019.NASDAQ_ITCH50"


def stream() -> bytes:
    return b"".join([
        system_event(T0, b"O"),                                                          # seq 1
        stock_directory(1, T0, "AAPL"),                                                  # 2
        add_order(1, T0 + 1 * NS, ref=10, side="B", shares=100, stock="AAPL", price=100_000),   # 3
        add_order(1, T0 + 2 * NS, ref=11, side="S", shares=50, stock="AAPL", price=100_500),    # 4
        order_executed(1, T0 + 3 * NS, ref=11, shares=20, match=1),                      # 5
        order_cancel(1, T0 + 4 * NS, ref=10, shares=40),                                 # 6
        order_replace(1, T0 + 5 * NS, old_ref=10, new_ref=13, shares=70, price=99_800),  # 7
        order_executed_price(1, T0 + 6 * NS, ref=13, shares=10, match=2, printable=True, price=99_700),  # 8
        order_delete(1, T0 + 7 * NS, ref=13),                                            # 9
        trade(1, T0 + 8 * NS, ref=77, side="B", shares=5, stock="AAPL", price=99_900, match=3),     # 10
        cross_trade(1, T0 + 9 * NS, shares=1000, stock="AAPL", price=100_000, match=4, cross_type="O"),  # 11
        broken_trade(1, T0 + 10 * NS, match=4),                                          # 12
        add_order_mpid(1, T0 + 11 * NS, ref=20, side="S", shares=7, stock="AAPL", price=101_000, mpid="NSDQ"),  # 13
        order_executed(1, T0 + 12 * NS, ref=999, shares=1, match=5),                     # 14 (unknown ref)
        system_event(T0 + 13 * NS, b"C"),                                                # 15
    ])


def write(tmp_path, data: bytes) -> str:
    p = tmp_path / NAME
    p.write_bytes(data)
    return str(p)


@pytest.fixture(params=[1, 64, 8 << 20])
def feed(tmp_path, request):
    return ib.open(write(tmp_path, stream()), chunk_bytes=request.param)


def gather(feed, tables):
    batches = list(feed.batches(tables=tables))
    out = {}
    for t in tables:
        if t == "symbols":
            continue
        cols = getattr(batches[0], t).keys()
        out[t] = {k: np.concatenate([getattr(b, t)[k] for b in batches]) for k in cols}
    return out


def chars(a: np.ndarray) -> list[str]:
    return [x.decode() for x in a.tolist()]


def test_trades_table(feed):
    tr = gather(feed, ("trades",))["trades"]
    assert chars(tr["kind"]) == ["E", "C", "P", "Q", "B"]
    assert tr["seq"].tolist() == [5, 8, 10, 11, 12]
    assert tr["price"][:4].tolist() == [10.05, 9.97, 9.99, 10.0] and math.isnan(tr["price"][4])
    assert tr["size"].tolist() == [20, 10, 5, 1000, 0]
    assert chars(tr["side"]) == ["S", "B", "N", "N", "N"]
    assert tr["order_id"].tolist() == [11, 13, 0, 0, 0]
    assert tr["match_number"].tolist() == [1, 2, 3, 4, 4]
    assert chars(tr["cross_type"]) == ["N", "N", "N", "O", "N"]
    assert tr["ts_event"].tolist() == [midnight_ns(dt.date(2019, 12, 30)) + T0 + k * NS for k in (3, 6, 8, 9, 10)]
    assert feed.stats["missing_ref"] == 1


def test_messages_table_carries_resting_side_price_and_remaining(feed):
    ms = gather(feed, ("messages",))["messages"]
    assert chars(ms["type"]) == ["A", "A", "E", "X", "U", "C", "D", "F", "E"]
    assert chars(ms["action"]) == ["A", "A", "F", "C", "M", "F", "C", "A", "F"]
    assert chars(ms["side"]) == ["B", "S", "S", "B", "B", "B", "B", "S", "N"]
    assert ms["seq"].tolist() == [3, 4, 5, 6, 7, 8, 9, 13, 14]
    assert ms["price"][:8].tolist() == [10.0, 10.05, 10.05, 10.0, 9.98, 9.97, 9.98, 10.1]
    assert math.isnan(ms["price"][8])
    assert ms["size"].tolist() == [100, 50, 20, 40, 70, 10, 60, 7, 1]
    assert ms["remaining"].tolist() == [100, 50, 30, 60, 70, 60, 0, 7, 0]
    assert ms["printable"].dtype == np.bool_ and ms["printable"].all()
    assert ms["order_id"].tolist() == [10, 11, 11, 10, 13, 13, 13, 20, 999]
    assert ms["old_order_id"].tolist() == [0, 0, 0, 0, 10, 0, 0, 0, 0]
    assert ms["mpid"].tolist() == [0, 0, 0, 0, 0, 0, 0, 1, 0]
    assert feed.mpids == ["", "NSDQ"]
    assert ms["locate"].tolist() == [1] * 9


def test_system_events_table(feed):
    ev = gather(feed, ("system_events",))["system_events"]
    assert chars(ev["event"]) == ["O", "C"]
    assert ev["seq"].tolist() == [1, 15]


def test_auction_and_halt_tables(tmp_path):
    data = b"".join([
        system_event(T0, b"O"),
        stock_directory(1, T0, "AAPL"),
        stock_directory(2, T0, "MSFT"),
        trading_action(1, T0 + 1 * NS, "AAPL", "H", "LUDP"),
        noii(1, T0 + 2 * NS, 5000, 1200, "B", "AAPL", 1_858_000, 1_857_500, 1_857_700, "O", "L"),
        reg_sho(1, T0 + 3 * NS, "AAPL", "1"),
        operational_halt(1, T0 + 4 * NS, "AAPL", "Q", "T"),
        luld_collar(1, T0 + 5 * NS, "AAPL", 1_857_000, 1_950_000, 1_764_000, 2),
        noii(2, T0 + 6 * NS, 0, 0, "N", "MSFT", 0, 0, 1_500_000, "C", " "),
        add_order(1, T0 + 7 * NS, ref=10, side="B", shares=100, stock="AAPL", price=1_857_000),
        system_event(T0 + 8 * NS, b"C"),
    ])
    f = ib.open(write(tmp_path, data), chunk_bytes=7)
    t = gather(f, ("noii", "halts", "reg_sho", "luld", "bbo"))
    m = midnight_ns(dt.date(2019, 12, 30))

    n = t["noii"]
    assert n["seq"].tolist() == [5, 9] and n["locate"].tolist() == [1, 2]
    assert n["ts_event"].tolist() == [m + T0 + 2 * NS, m + T0 + 6 * NS]
    assert n["paired"].tolist() == [5000, 0] and n["imbalance"].tolist() == [1200, 0]
    assert chars(n["direction"]) == ["B", "N"] and chars(n["cross_type"]) == ["O", "C"]
    assert chars(n["variation"]) == ["L", " "]
    assert n["far_px"].tolist()[0] == 185.8 and n["near_px"].tolist()[0] == 185.75
    assert n["ref_px"].tolist() == [185.77, 150.0]
    assert math.isnan(n["far_px"][1]) and math.isnan(n["near_px"][1])

    h = t["halts"]
    assert h["seq"].tolist() == [4, 7] and chars(h["kind"]) == ["H", "h"]
    assert chars(h["state"]) == ["H", "T"] and chars(h["market"]) == ["N", "Q"]
    assert [x.decode() for x in h["reason"].tolist()] == ["LUDP", "    "]

    r = t["reg_sho"]
    assert r["seq"].tolist() == [6] and chars(r["action"]) == ["1"]

    c = t["luld"]
    assert c["seq"].tolist() == [8] and c["extension"].tolist() == [2]
    assert (c["ref_px"].tolist(), c["upper_px"].tolist(), c["lower_px"].tolist()) == ([185.7], [195.0], [176.4])

    assert t["bbo"]["seq"].tolist() == [10]
    assert f.stats["messages"] == 11 and f.stats["unknown"] == 0

    only = gather(ib.open(write(tmp_path, data)), ("noii",))
    assert only["noii"]["seq"].tolist() == [5, 9]


def test_tables_are_independent_and_bbo_unchanged(feed):
    out = gather(feed, ("bbo", "trades", "messages", "system_events"))
    assert len(out["trades"]["seq"]) == 5 and len(out["messages"]["seq"]) == 9
    bbo = out["bbo"]
    assert bbo["seq"].tolist()[:3] == [3, 4, 5]
    assert bbo["bid_px"][0] == 10.0 and math.isnan(bbo["ask_px"][0])


def test_fixed_prices_in_every_table(tmp_path):
    out = gather(ib.open(write(tmp_path, stream()), price_type="fixed"), ("trades", "messages"))
    assert out["trades"]["price"].tolist() == [100_500, 99_700, 99_900, 100_000, 0]
    assert out["messages"]["price"][:2].tolist() == [100_000, 100_500]


def test_to_polars_casts_chars_and_timestamps(feed):
    pl = pytest.importorskip("polars")
    out = gather(feed, ("trades", "messages"))
    tr = ib.to_polars(out["trades"])
    assert tr.schema["kind"] == pl.Utf8 and tr["kind"].to_list() == ["E", "C", "P", "Q", "B"]
    assert tr.schema["ts_event"] == pl.Datetime("ns", "UTC")
    ms = ib.to_polars(out["messages"])
    assert ms.schema["action"] == pl.Utf8 and ms["remaining"].dtype == pl.UInt32
    assert ms.schema["printable"] == pl.Boolean


def test_rows_the_book_did_not_apply_say_so(tmp_path):
    data = b"".join([
        add_order(1, T0 + 1 * NS, ref=0, side="B", shares=100, stock="AAPL", price=100_000),   # rejected: ref 0
        add_order(1, T0 + 2 * NS, ref=5, side="B", shares=100, stock="AAPL", price=0),         # rejected: price 0
        add_order(1, T0 + 3 * NS, ref=6, side="B", shares=100, stock="AAPL", price=100_000),
        add_order(1, T0 + 4 * NS, ref=6, side="S", shares=30, stock="AAPL", price=100_500),    # dup: evicts ref 6
        order_executed(1, T0 + 5 * NS, ref=6, shares=500, match=1),                            # clamped to 30
        order_replace(1, T0 + 6 * NS, old_ref=404, new_ref=7, shares=10, price=100_000),      # missing old
        add_order(1, T0 + 7 * NS, ref=8, side="B", shares=40, stock="AAPL", price=99_000),
        add_order(1, T0 + 8 * NS, ref=9, side="S", shares=50, stock="AAPL", price=99_500),
        order_replace(1, T0 + 9 * NS, old_ref=8, new_ref=9, shares=45, price=99_100),         # live new_ref: evicts 9
        order_executed_price(1, T0 + 10 * NS, ref=9, shares=1, match=2, printable=False, price=99_100),
        order_cancel(1, T0 + 11 * NS, ref=9, shares=100),                                      # clamped to 44
        system_event(T0 + 12 * NS, b"C"),
    ])
    feed = ib.open(write(tmp_path, data))
    out = gather(feed, ("bbo", "trades", "messages"))
    ms = out["messages"]
    assert chars(ms["type"]) == ["A", "A", "A", "A", "E", "U", "A", "A", "U", "C", "X"]
    assert ms["remaining"].tolist() == [0, 0, 100, 30, 0, 0, 40, 50, 45, 44, 0]
    assert chars(ms["side"]) == ["B", "B", "B", "S", "S", "N", "B", "S", "B", "B", "B"]
    assert ms["size"].tolist() == [100, 100, 100, 30, 500, 10, 40, 50, 45, 1, 100]
    assert ms["printable"].tolist() == [True] * 9 + [False, True]
    assert chars(out["trades"]["kind"]) == ["E"]
    assert out["trades"]["size"].tolist() == [500]
    s = feed.stats
    assert (s["rejected"], s["dup_ref"], s["missing_ref"], s["clamped"], s["live_orders"]) == (2, 2, 1, 2, 0)
    bbo = out["bbo"]
    assert bbo["seq"].tolist() == [3, 4, 5, 7, 8, 9, 10, 11]
    assert math.isnan(bbo["bid_px"][-1]) and math.isnan(bbo["ask_px"][-1])


def replay(ms):
    orders: dict[int, tuple[int, str, float, int]] = {}
    last: dict[int, tuple] = {}
    out = []
    for i in range(len(ms["seq"])):
        side = ms["side"][i].decode()
        if side == "N":
            continue
        typ = ms["type"][i].decode()
        oid = int(ms["order_id"][i])
        loc = int(ms["locate"][i])
        rem = int(ms["remaining"][i])
        touched = []
        if typ in "AF":
            if rem:
                evicted = orders.pop(oid, None)
                if evicted and evicted[0] != loc:
                    touched.append(evicted[0])
                orders[oid] = (loc, side, float(ms["price"][i]), rem)
        elif typ == "U":
            orders.pop(int(ms["old_order_id"][i]), None)
            evicted = orders.pop(oid, None)
            if evicted and evicted[0] != loc:
                touched.append(evicted[0])
            if rem:
                orders[oid] = (loc, side, float(ms["price"][i]), rem)
        else:
            if rem:
                loc, side, px, _ = orders[oid]
                orders[oid] = (loc, side, px, rem)
            else:
                orders.pop(oid, None)
        touched.append(loc)
        for book in touched:
            bids = [(px, q) for l, s, px, q in orders.values() if l == book and s == "B"]
            asks = [(px, q) for l, s, px, q in orders.values() if l == book and s == "S"]
            best_bid = max(p for p, _ in bids) if bids else None
            best_ask = min(p for p, _ in asks) if asks else None
            top = (
                best_bid, sum(q for p, q in bids if p == best_bid),
                best_ask, sum(q for p, q in asks if p == best_ask),
            )
            if last.get(book) != top:
                last[book] = top
                out.append((int(ms["seq"][i]), book) + top)
    return out


def bbo_rows(bbo):
    rows = []
    for i in range(len(bbo["seq"])):
        bid = None if math.isnan(bbo["bid_px"][i]) else float(bbo["bid_px"][i])
        ask = None if math.isnan(bbo["ask_px"][i]) else float(bbo["ask_px"][i])
        rows.append((int(bbo["seq"][i]), int(bbo["locate"][i]), bid, int(bbo["bid_sz"][i]), ask, int(bbo["ask_sz"][i])))
    return rows


def random_day(seed: int, n: int) -> bytes:
    rng = random.Random(seed)
    parts = [system_event(T0, b"O")] + [stock_directory(loc, T0, f"SYM{loc}") for loc in (1, 2, 3)]
    live: dict[int, tuple[int, str, int, int]] = {}
    dead: list[int] = []
    next_ref = 100
    ts = T0
    for _ in range(n):
        ts += rng.randint(1, 5_000_000)
        r = rng.random()
        if r < 0.45 or not live:
            loc = rng.choice((1, 2, 3))
            side = rng.choice("BS")
            px = 100_000 + rng.randint(-40, 40) * 100
            qty = rng.choice((100, 200, 300, 500))
            ref = next_ref
            next_ref += 1
            if rng.random() < 0.02 and live:
                ref = rng.choice(list(live))
            parts.append(add_order(loc, ts, ref=ref, side=side, shares=qty, stock=f"SYM{loc}", price=px))
            live[ref] = (loc, side, px, qty)
        elif r < 0.65:
            ref = rng.choice(list(live))
            loc, side, px, qty = live[ref]
            take = rng.choice((qty // 2 or 1, qty, qty + 50))
            parts.append(order_executed(loc, ts, ref=ref, shares=take, match=next_ref))
            next_ref += 1
            if take >= qty:
                dead.append(ref)
                del live[ref]
            else:
                live[ref] = (loc, side, px, qty - take)
        elif r < 0.78:
            ref = rng.choice(list(live))
            loc, side, px, qty = live[ref]
            parts.append(order_delete(loc, ts, ref=ref))
            dead.append(ref)
            del live[ref]
        elif r < 0.88:
            ref = rng.choice(list(live))
            loc, side, px, qty = live[ref]
            take = rng.choice((qty // 3 or 1, qty + 10))
            parts.append(order_cancel(loc, ts, ref=ref, shares=take))
            if take >= qty:
                dead.append(ref)
                del live[ref]
            else:
                live[ref] = (loc, side, px, qty - take)
        elif r < 0.96:
            ref = rng.choice(list(live))
            loc, side, px, qty = live[ref]
            new_px = px + rng.randint(-3, 3) * 100
            new_qty = rng.choice((qty, qty + 100, 50))
            new_ref = next_ref
            next_ref += 1
            if rng.random() < 0.05 and len(live) > 1:
                new_ref = rng.choice([k for k in live if k != ref])
            parts.append(order_replace(loc, ts, old_ref=ref, new_ref=new_ref, shares=new_qty, price=new_px))
            del live[ref]
            live[new_ref] = (loc, side, new_px, new_qty)
        elif dead:
            ref = rng.choice(dead)
            parts.append(rng.choice((order_delete(1, ts, ref=ref), order_executed(1, ts, ref=ref, shares=10, match=next_ref))))
            next_ref += 1
    parts.append(system_event(ts + NS, b"C"))
    return b"".join(parts)


@pytest.mark.parametrize("seed", [1, 7, 2026])
def test_messages_replay_reproduces_bbo(tmp_path, seed):
    feed = ib.open(write(tmp_path, random_day(seed, 20_000)), chunk_bytes=4096)
    out = gather(feed, ("bbo", "messages"))
    assert feed.stats["messages"] > 20_000
    assert feed.stats["missing_ref"] > 0 and feed.stats["clamped"] > 0 and feed.stats["dup_ref"] > 0
    assert replay(out["messages"]) == bbo_rows(out["bbo"])
