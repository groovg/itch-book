import datetime as dt
import gc
import gzip
import math
import threading

import numpy as np
import pytest

import itch_book as ib
from itch_book._dates import midnight_ns, session_date

from itch_stream import (
    add_order,
    add_order_mpid,
    end_of_session,
    order_delete,
    order_executed,
    stock_directory,
    system_event,
    unknown_message,
)

DAY = dt.date(2019, 12, 30)
NS = 1_000_000_000
NAME = "12302019.NASDAQ_ITCH50"


def messages() -> list[bytes]:
    return [
        system_event(34_200 * NS, b"O"),
        stock_directory(1, 34_200 * NS, "AAPL"),
        stock_directory(2, 34_200 * NS, "MSFT", round_lot=10),
        add_order(1, 34_201 * NS, ref=10, side="B", shares=100, stock="AAPL", price=100_000),
        add_order(1, 34_202 * NS, ref=11, side="S", shares=50, stock="AAPL", price=100_500),
        add_order(1, 34_203 * NS, ref=12, side="B", shares=200, stock="AAPL", price=99_900),
        order_executed(1, 34_204 * NS, ref=11, shares=20, match=1),
        order_delete(1, 34_205 * NS, ref=10),
        add_order_mpid(2, 34_206 * NS, ref=20, side="S", shares=7, stock="MSFT", price=1_500_000, mpid="NSDQ"),
        order_delete(1, 34_207 * NS, ref=11),
        system_event(57_600 * NS, b"C"),
    ]


def stream(trailer: bytes = b"") -> bytes:
    return b"".join(messages()) + trailer


def write(tmp_path, data: bytes, gz: bool, name: str = NAME):
    p = tmp_path / (name + (".gz" if gz else ""))
    p.write_bytes(gzip.compress(data) if gz else data)
    return str(p)


@pytest.fixture(params=["raw", "gz"])
def path(tmp_path, request):
    return write(tmp_path, stream(), gz=request.param == "gz")


def collect(path, tables=("bbo", "symbols"), **kwargs):
    rows = kwargs.pop("rows", 1_000_000)
    feed = ib.open(path, **kwargs)
    batches = list(feed.batches(tables=tables, rows=rows))
    out = {}
    if "bbo" in tables:
        out["bbo"] = {k: np.concatenate([b.bbo[k] for b in batches]) for k in ib.BBO_COLUMNS}
    if "symbols" in tables:
        out["symbols"] = {c: sum((b.symbols[c] for b in batches), []) for c in ib.SYMBOL_COLUMNS}
    return feed, out, batches


@pytest.mark.parametrize("chunk_bytes", [1, 7, 64, 8 << 20])
def test_bbo_rows_follow_book_changes(path, chunk_bytes):
    feed, out, _ = collect(path, chunk_bytes=chunk_bytes)
    bbo = out["bbo"]
    assert feed.stats["messages"] == 11
    assert feed.stats["last_event"] == "C"
    assert feed.stats["end_of_session"] is False
    assert feed.stats["pending_bytes"] == 0
    assert feed.stats["crossed_books"] == 0
    assert feed.stats["live_orders"] == 2

    assert bbo["seq"].tolist() == [4, 5, 7, 8, 9, 10]
    assert bbo["locate"].tolist() == [1, 1, 1, 1, 2, 1]
    assert bbo["bid_px"][:2].tolist() == [10.0, 10.0]
    assert math.isnan(bbo["ask_px"][0])
    assert bbo["ask_px"][1] == 10.05 and bbo["ask_sz"][1] == 50
    assert bbo["ask_sz"][2] == 30 and bbo["ask_ct"][2] == 1
    assert bbo["bid_px"][3] == 9.99 and bbo["bid_sz"][3] == 200
    assert bbo["ask_px"][4] == 150.0 and bbo["ask_sz"][4] == 7 and math.isnan(bbo["bid_px"][4])
    assert math.isnan(bbo["ask_px"][5]) and bbo["ask_sz"][5] == 0 and bbo["ask_ct"][5] == 0
    assert bbo["bid_sz"].dtype == np.uint32

    midnight = midnight_ns(DAY)
    assert bbo["ts_event"].dtype == np.int64
    assert bbo["ts_event"].tolist() == [midnight + t * NS for t in (34_201, 34_202, 34_204, 34_205, 34_206, 34_207)]

    assert out["symbols"]["symbol"] == ["AAPL", "MSFT"]
    assert out["symbols"]["round_lot_size"] == [100, 10]
    assert out["symbols"]["locate"] == [1, 2]


def test_fixed_prices_are_raw_mantissas(path):
    _, out, _ = collect(path, price_type="fixed")
    bbo = out["bbo"]
    assert bbo["bid_px"].dtype == np.int64 and bbo["ask_px"].dtype == np.int64
    assert bbo["bid_px"].tolist() == [100_000, 100_000, 100_000, 99_900, 0, 99_900]
    assert bbo["ask_px"].tolist() == [0, 100_500, 100_500, 100_500, 1_500_000, 0]


def test_small_batches_keep_every_row_and_symbol(path):
    _, out, batches = collect(path, rows=1, chunk_bytes=1)
    assert len(batches) >= 6
    assert all(hasattr(b, "bbo") and hasattr(b, "symbols") for b in batches)
    assert len(out["bbo"]["seq"]) == 6
    assert out["symbols"]["symbol"] == ["AAPL", "MSFT"]


def test_zero_length_trailer_ends_the_session(tmp_path):
    p = write(tmp_path, stream(trailer=end_of_session() + b"garbage after the end"), gz=False)
    feed, out, _ = collect(p)
    assert feed.stats["end_of_session"] is True
    assert feed.stats["messages"] == 11
    assert len(out["bbo"]["seq"]) == 6


def test_seq_counts_decoded_messages_only(tmp_path):
    data = b"".join([unknown_message(34_200 * NS), unknown_message(34_200 * NS)] + messages())
    feed, out, _ = collect(write(tmp_path, data, gz=False))
    assert feed.stats["unknown"] == 2
    assert feed.stats["messages"] == 11
    assert out["bbo"]["seq"][0] == 4


def test_empty_and_non_itch_inputs_are_rejected(tmp_path):
    with pytest.raises(ValueError, match="not an ITCH"):
        collect(write(tmp_path, b"", gz=False))
    with pytest.raises(ValueError, match="not an ITCH"):
        collect(write(tmp_path, b"A" * 60_000, gz=True))


def test_truncated_gzip_raises(tmp_path):
    whole = gzip.compress(stream())
    p = tmp_path / (NAME + ".gz")
    p.write_bytes(whole[:-40])
    with pytest.raises(EOFError):
        collect(str(p))


def test_multi_member_gzip_is_read_completely(tmp_path):
    msgs = messages()
    data = gzip.compress(b"".join(msgs[:5])) + gzip.compress(b"".join(msgs[5:]))
    p = tmp_path / (NAME + ".gz")
    p.write_bytes(data)
    feed, out, _ = collect(str(p))
    assert feed.stats["messages"] == 11
    assert len(out["bbo"]["seq"]) == 6


def test_abandoned_generator_stops_the_reader(path):
    before = threading.active_count()
    feed = ib.open(path, chunk_bytes=1)
    g = feed.batches(rows=1)
    next(g)
    g.close()
    gc.collect()
    assert threading.active_count() == before
    with ib.open(path) as f:
        list(f.batches())
    assert threading.active_count() == before


def test_to_polars_types(path):
    pl = pytest.importorskip("polars")
    _, out, _ = collect(path)
    df = ib.to_polars(out["bbo"])
    assert df.schema["ts_event"] == pl.Datetime("ns", "UTC")
    assert df.schema["bid_px"] == pl.Float64 and df.schema["bid_sz"] == pl.UInt32
    assert df.height == 6


def test_unknown_table_is_rejected(path):
    with pytest.raises(ValueError, match="unknown tables"):
        list(ib.open(path).batches(tables=("quotes",)))


def test_session_date_from_filename():
    assert session_date("12302019.NASDAQ_ITCH50.gz") == dt.date(2019, 12, 30)
    assert session_date("/data/S121225-v50.txt.gz") == dt.date(2025, 12, 12)
    assert session_date("20190530.BX_ITCH_50.gz") == dt.date(2019, 5, 30)
    assert session_date("whatever.bin") is None
    with pytest.raises(ValueError):
        ib.open("whatever.bin")


def test_midnight_is_new_york_local():
    assert midnight_ns(dt.date(2019, 12, 30)) == 1_577_682_000 * NS
    assert midnight_ns(dt.date(2019, 7, 30)) == 1_564_459_200 * NS
