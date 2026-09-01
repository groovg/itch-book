import math

import numpy as np
import pytest

import itch_book as ib

from itch_stream import add_order, order_cancel, order_delete, stock_directory, system_event, trade
from test_tables import bbo_rows, random_day

NS = 1_000_000_000
T0 = 34_200 * NS
NAME = "12302019.NASDAQ_ITCH50"


def stream() -> bytes:
    return b"".join([
        system_event(T0, b"O"),                                                                  # 1
        stock_directory(1, T0, "AAPL"),                                                          # 2
        stock_directory(2, T0, "MSFT"),                                                          # 3
        add_order(1, T0 + 1 * NS, ref=10, side="B", shares=100, stock="AAPL", price=100_000),    # 4
        add_order(1, T0 + 2 * NS, ref=11, side="B", shares=200, stock="AAPL", price=99_900),     # 5
        add_order(1, T0 + 3 * NS, ref=12, side="B", shares=300, stock="AAPL", price=99_800),     # 6  third level: invisible at depth 2
        add_order(1, T0 + 4 * NS, ref=13, side="S", shares=50, stock="AAPL", price=100_500),     # 7
        add_order(2, T0 + 5 * NS, ref=20, side="S", shares=7, stock="MSFT", price=1_500_000),    # 8
        order_cancel(1, T0 + 6 * NS, ref=12, shares=100),                                        # 9  still invisible
        order_delete(1, T0 + 7 * NS, ref=10),                                                    # 10 level 3 becomes level 2
        trade(2, T0 + 8 * NS, ref=0, side="B", shares=5, stock="MSFT", price=1_499_000, match=1),  # 11
        system_event(T0 + 9 * NS, b"C"),                                                         # 12
    ])


def write(tmp_path, data: bytes = None) -> str:
    p = tmp_path / NAME
    p.write_bytes(stream() if data is None else data)
    return str(p)


def gather(feed, tables, **kwargs):
    batches = list(feed.batches(tables=tables, **kwargs))
    out = {}
    for t in tables:
        if t == "symbols":
            out[t] = {c: sum((b.symbols[c] for b in batches), []) for c in ib.SYMBOL_COLUMNS}
            continue
        cols = getattr(batches[0], t).keys()
        out[t] = {k: np.concatenate([getattr(b, t)[k] for b in batches]) for k in cols}
    return out


def test_depth_rows_only_when_the_visible_levels_change(tmp_path):
    feed = ib.open(write(tmp_path), chunk_bytes=64)
    d = gather(feed, ("depth",), depth=2)["depth"]
    assert list(d.keys()) == ["ts_event", "seq", "locate"] + [
        f"{s}_{k}_{i:02d}" for i in range(2) for s in ("bid", "ask") for k in ("px", "sz", "ct")
    ]
    assert d["seq"].tolist() == [4, 5, 7, 8, 10]
    assert d["locate"].tolist() == [1, 1, 1, 2, 1]
    assert d["bid_px_00"].tolist()[:3] == [10.0, 10.0, 10.0]
    assert math.isnan(d["bid_px_01"][0]) and d["bid_px_01"][1] == 9.99
    assert d["bid_sz_01"][1] == 200 and d["bid_ct_01"][1] == 1
    assert d["ask_px_00"][2] == 10.05 and d["ask_sz_00"][2] == 50
    assert math.isnan(d["bid_px_00"][3]) and d["ask_px_00"][3] == 150.0
    assert d["bid_px_00"][4] == 9.99 and d["bid_px_01"][4] == 9.98 and d["bid_sz_01"][4] == 200
    assert d["bid_sz_00"].dtype == np.uint32
    assert feed.stats["selected"] is None


@pytest.mark.parametrize("symbols", [("MSFT",), "MSFT", ("msft",), ("MSFT", "msft")])
def test_symbols_filter_applies_to_every_row_table(tmp_path, symbols):
    feed = ib.open(write(tmp_path))
    out = gather(feed, ("bbo", "trades", "messages", "depth", "symbols", "system_events"), depth=1, symbols=symbols)
    assert set(out["bbo"]["locate"].tolist()) == {2}
    assert out["trades"]["locate"].tolist() == [2]
    assert out["messages"]["locate"].tolist() == [2]
    assert out["depth"]["locate"].tolist() == [2]
    assert out["symbols"]["symbol"] == ["AAPL", "MSFT"]
    assert len(out["system_events"]["seq"]) == 2
    assert feed.stats["selected"] == 1 and feed.stats["adds"] == 5


def test_symbol_filter_edge_cases(tmp_path):
    feed = ib.open(write(tmp_path))
    with pytest.warns(UserWarning, match="1 of 1 symbols not in the stock directory"):
        out = gather(feed, ("bbo",), symbols=("NOPE",))
    assert len(out["bbo"]["seq"]) == 0 and feed.stats["selected"] == 0
    with pytest.raises(ValueError, match="8 characters"):
        list(feed.batches(symbols=("TOOLONGNAME",)))


def test_no_phantom_depth_row_before_the_book_changes(tmp_path):
    data = b"".join([
        stock_directory(1, T0, "AAPL"),
        add_order(1, T0 + NS, ref=0, side="B", shares=100, stock="AAPL", price=100_000),      # rejected
        order_delete(1, T0 + 2 * NS, ref=404),                                                 # missing
        add_order(1, T0 + 3 * NS, ref=1, side="B", shares=100, stock="AAPL", price=100_000),
        system_event(T0 + 4 * NS, b"C"),
    ])
    d = gather(ib.open(write(tmp_path, data)), ("depth",), depth=3)["depth"]
    assert d["seq"].tolist() == [4]


def test_eviction_across_books_emits_rows_for_both(tmp_path):
    data = b"".join([
        stock_directory(1, T0, "AAPL"),
        stock_directory(2, T0, "MSFT"),
        add_order(1, T0 + NS, ref=10, side="B", shares=100, stock="AAPL", price=100_000),
        add_order(2, T0 + 2 * NS, ref=10, side="S", shares=5, stock="MSFT", price=1_500_000),  # dup ref, other book
        system_event(T0 + 3 * NS, b"C"),
    ])
    out = gather(ib.open(write(tmp_path, data)), ("bbo", "depth"), depth=1)
    assert out["bbo"]["seq"].tolist() == [3, 4, 4]
    assert out["bbo"]["locate"].tolist() == [1, 1, 2]
    assert math.isnan(out["bbo"]["bid_px"][1]) and out["bbo"]["ask_px"][2] == 150.0
    assert out["depth"]["locate"].tolist() == [1, 1, 2]
    assert math.isnan(out["depth"]["bid_px_00"][1])


@pytest.mark.parametrize("seed", [1, 7, 2026])
def test_depth_one_equals_bbo_and_depth_seq_is_a_message_seq(tmp_path, seed):
    feed = ib.open(write(tmp_path, random_day(seed, 20_000)), chunk_bytes=4096)
    out = gather(feed, ("bbo", "depth", "messages"), rows=50, depth=1)
    d, b = out["depth"], out["bbo"]
    assert d["seq"].tolist() == b["seq"].tolist() and d["locate"].tolist() == b["locate"].tolist()
    for k in ("px", "sz", "ct"):
        for side in ("bid", "ask"):
            np.testing.assert_array_equal(d[f"{side}_{k}_00"], b[f"{side}_{k}"])
    msg_seqs = set(out["messages"]["seq"].tolist())
    assert set(d["seq"].tolist()) <= msg_seqs
    rows = list(zip(d["seq"].tolist(), d["locate"].tolist(), d["bid_px_00"].tolist(), d["bid_sz_00"].tolist()))
    assert all(a != b for a, b in zip(rows, rows[1:]))
