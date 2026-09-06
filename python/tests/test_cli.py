import datetime as dt
import gzip
import hashlib
import http.server
import io
import json
import re
import threading

import pytest

pa = pytest.importorskip("pyarrow")
pq = pytest.importorskip("pyarrow.parquet")

from itch_book import cli
from itch_book._dates import midnight_ns

from itch_stream import (
    add_order,
    broken_trade,
    cross_trade,
    noii,
    operational_halt,
    order_cancel,
    order_delete,
    order_executed,
    order_replace,
    stock_directory,
    system_event,
    trade,
    trading_action,
)

NS = 1_000_000_000
T0 = 34_200 * NS


def stream(missing_ref: bool = False) -> bytes:
    parts = [
        system_event(T0, b"O"),
        stock_directory(1, T0, "AAPL"),
        stock_directory(2, T0, "MSFT"),
        add_order(1, T0 + 1 * NS, ref=10, side="B", shares=100, stock="AAPL", price=100_000),
        add_order(1, T0 + 2 * NS, ref=11, side="S", shares=50, stock="AAPL", price=100_500),
        order_executed(1, T0 + 3 * NS, ref=11, shares=20, match=1),
        add_order(2, T0 + 4 * NS, ref=20, side="S", shares=7, stock="MSFT", price=1_500_000),
        trade(2, T0 + 5 * NS, ref=0, side="B", shares=5, stock="MSFT", price=1_499_000, match=3),
        cross_trade(1, T0 + 6 * NS, shares=1000, stock="AAPL", price=100_000, match=4, cross_type="O"),
        broken_trade(1, T0 + 7 * NS, match=4),
        order_delete(1, T0 + 8 * NS, ref=10),
    ]
    if missing_ref:
        parts.append(order_delete(1, T0 + 9 * NS, ref=999))
    parts.append(system_event(T0 + 10 * NS, b"C"))
    return b"".join(parts)


@pytest.fixture
def day(tmp_path):
    p = tmp_path / "12302019.NASDAQ_ITCH50"
    p.write_bytes(stream())
    return str(p)


def files(out):
    return sorted(p.name for p in out.iterdir())


def test_convert_default_tables(day, tmp_path, capsys):
    out = tmp_path / "out"
    assert cli.main(["convert", day, str(out), "--rows", "2"]) == 0
    assert "inferred from the filename" in capsys.readouterr().err
    assert files(out) == ["bbo.parquet", "symbols.parquet", "system_events.parquet", "trades.parquet"]

    bbo = pq.read_table(out / "bbo.parquet")
    assert bbo.schema.field("ts_event").type == pa.timestamp("ns", tz="UTC")
    assert pa.types.is_dictionary(bbo.schema.field("symbol").type)
    assert bbo.column("symbol").to_pylist() == ["AAPL", "AAPL", "AAPL", "MSFT", "AAPL"]
    assert bbo.column("bid_px").to_pylist()[0] == 10.0

    bbo_file = pq.ParquetFile(out / "bbo.parquet")
    sorting = bbo_file.metadata.row_group(0).sorting_columns
    names = bbo_file.schema_arrow.names
    assert [names[s.column_index] for s in sorting] == ["ts_event", "seq"]

    trades_file = pq.ParquetFile(out / "trades.parquet")
    assert trades_file.num_row_groups == 1
    assert [names[s.column_index] for s in trades_file.metadata.row_group(0).sorting_columns] == ["ts_event", "seq"]
    trades = trades_file.read()
    assert trades.column("kind").to_pylist() == ["E", "P", "Q", "B"]
    assert trades.column("broken").to_pylist() == [False, False, True, False]
    assert trades.column("symbol").to_pylist() == ["AAPL", "MSFT", "AAPL", "AAPL"]
    assert not (out / "trades.parquet.part").exists()

    meta = pq.read_metadata(out / "trades.parquet").metadata
    assert meta[b"itch_book.schema"] == b"1"
    assert meta[b"itch_book.session_date"] == b"2019-12-30"
    assert meta[b"itch_book.source"] == b"12302019.NASDAQ_ITCH50"
    assert meta[b"itch_book.tables"] == b"bbo,trades"
    assert len(meta[b"itch_book.source_md5"]) == 32
    assert b"itch_book.depth" not in meta
    stats = json.loads(meta[b"itch_book.stats"])
    assert stats["messages"] == 12 and stats["crossed_books"] == 0

    symbols = pq.read_table(out / "symbols.parquet")
    assert symbols.column("symbol").to_pylist() == ["AAPL", "MSFT"]
    assert symbols.schema.field("locate").type == pa.uint16()
    assert symbols.schema.field("round_lot_size").type == pa.uint32()
    events = pq.read_table(out / "system_events.parquet")
    assert events.column("event").to_pylist() == ["O", "C"]


def test_convert_auction_and_halt_tables(tmp_path):
    p = tmp_path / "12302019.NASDAQ_ITCH50"
    p.write_bytes(b"".join([
        system_event(T0, b"O"),
        stock_directory(1, T0, "AAPL"),
        trading_action(1, T0 + NS, "AAPL", "H", "LUDP"),
        noii(1, T0 + 2 * NS, 5000, 1200, "B", "AAPL", 1_858_000, 1_857_500, 1_857_700, "O", "L"),
        system_event(T0 + 3 * NS, b"C"),
    ]))
    out = tmp_path / "out"
    assert cli.main(["convert", str(p), str(out), "--tables", "noii,halts"]) == 0
    assert files(out) == ["halts.parquet", "noii.parquet", "symbols.parquet", "system_events.parquet"]
    halts = pq.read_table(out / "halts.parquet")
    assert halts.column("reason").to_pylist() == ["LUDP"]
    assert halts.column("kind").to_pylist() == ["H"] and halts.column("symbol").to_pylist() == ["AAPL"]
    n = pq.read_table(out / "noii.parquet")
    assert n.column("ref_px").to_pylist() == [185.77] and n.column("direction").to_pylist() == ["B"]
    assert n.schema.field("paired").type == pa.uint64()


def test_lobster_export(tmp_path, capsys):
    p = tmp_path / "12302019.NASDAQ_ITCH50"
    p.write_bytes(b"".join([
        system_event(T0, b"O"),
        stock_directory(1, T0, "AAPL"),
        stock_directory(2, T0, "MSFT"),
        add_order(1, T0 - 5 * NS, ref=9, side="B", shares=10, stock="AAPL", price=990_000),
        add_order(1, T0 + 1 * NS, ref=10, side="B", shares=100, stock="AAPL", price=1_000_000),
        add_order(1, T0 + 2 * NS, ref=11, side="S", shares=50, stock="AAPL", price=1_005_000),
        order_executed(1, T0 + 3 * NS, ref=11, shares=20, match=1),
        order_cancel(1, T0 + 4 * NS, ref=10, shares=30),
        order_replace(1, T0 + 5 * NS, old_ref=10, new_ref=12, shares=60, price=999_000),
        add_order(2, T0 + 5 * NS + 1, ref=20, side="S", shares=7, stock="MSFT", price=1_500_000),
        order_delete(1, T0 + 6 * NS, ref=12),
        trade(1, T0 + 7 * NS, ref=0, side="B", shares=5, stock="AAPL", price=1_001_000, match=3),
        trading_action(1, T0 + 8 * NS, "AAPL", "H", "LUDP"),
        order_delete(1, T0 + 9 * NS, ref=999),
        stock_directory(3, T0 + 9 * NS, "ZZZ"),
        operational_halt(3, T0 + 9 * NS + 1, "ZZZ", "Q", "H"),
        system_event(T0 + 10 * NS, b"C"),
    ]))
    out = tmp_path / "lob"
    assert cli.main(["lobster", str(p), str(out), "--symbols", "aapl,zzz", "--levels", "2"]) == 0
    assert "2 symbols" in capsys.readouterr().out
    assert files(out) == [
        "AAPL_2019-12-30_34200000_57600000_message_2.csv", "AAPL_2019-12-30_34200000_57600000_orderbook_2.csv",
        "ZZZ_2019-12-30_34200000_57600000_message_2.csv", "ZZZ_2019-12-30_34200000_57600000_orderbook_2.csv",
    ]
    zzz = (out / files(out)[2]).read_text().splitlines()
    assert zzz == ["34209.000000001,7,0,0,-1,-1"]
    assert (out / files(out)[3]).read_text().splitlines() == ["9999999999,0,-9999999999,0,9999999999,0,-9999999999,0"]
    msg = [line.split(",") for line in (out / files(out)[0]).read_text().splitlines()]
    assert [(r[1], r[2], r[3], r[4], r[5]) for r in msg] == [
        ("1", "10", "100", "1000000", "1"),
        ("1", "11", "50", "1005000", "-1"),
        ("4", "11", "20", "1005000", "-1"),
        ("2", "10", "30", "1000000", "1"),
        ("3", "10", "70", "1000000", "1"),
        ("1", "12", "60", "999000", "1"),
        ("3", "12", "60", "999000", "1"),
        ("5", "0", "5", "1001000", "1"),
        ("7", "0", "0", "-1", "-1"),
    ]
    assert msg[0][0] == "34201" and msg[2][0] == "34203"
    book = [line.split(",") for line in (out / files(out)[1]).read_text().splitlines()]
    assert len(book) == 9
    assert book[0] == ["9999999999", "0", "1000000", "100", "9999999999", "0", "990000", "10"]
    assert book[1][:4] == ["1005000", "50", "1000000", "100"]
    assert book[3][:4] == ["1005000", "30", "1000000", "70"]
    assert book[4][:4] == ["1005000", "30", "999000", "60"] and book[5] == book[4]
    assert book[6][:4] == ["1005000", "30", "990000", "10"] and book[6][4:] == ["9999999999", "0", "-9999999999", "0"]
    assert book[7] == book[6] and book[8] == book[6]

    assert cli.main(["lobster", str(p), str(out)]) == 2
    assert cli.main(["lobster", str(p), str(out), "--symbols", "ZZZZ"]) == 1


def test_writer_thread_error_leaves_nothing_behind(day, tmp_path, monkeypatch, capsys):
    def boom(table, lookup):
        raise ValueError("arrow conversion failed")

    monkeypatch.setattr(cli, "to_arrow", boom)
    out = tmp_path / "out"
    assert cli.main(["convert", day, str(out)]) == 2
    assert "arrow conversion failed" in capsys.readouterr().err
    assert files(out) == []


def test_convert_messages_with_symbol_filter_and_date_override(day, tmp_path, capsys):
    out = tmp_path / "out"
    assert cli.main(["convert", day, str(out), "--tables", "messages,depth", "--symbols", " msft ,MSFT",
                     "--depth", "2", "--date", "2020-01-02", "--no-md5"]) == 0
    assert "inferred" not in capsys.readouterr().err
    ms = pq.read_table(out / "messages.parquet")
    assert ms.column("symbol").to_pylist() == ["MSFT"]
    assert ms.column("action").to_pylist() == ["A"]
    ts = ms.column("ts_event").cast(pa.int64()).to_pylist()[0]
    assert ts == midnight_ns(dt.date(2020, 1, 2)) + T0 + 4 * NS
    depth = pq.read_table(out / "depth.parquet")
    assert "ask_px_01" in depth.column_names and depth.num_rows == 1
    meta = pq.read_metadata(out / "messages.parquet").metadata
    assert meta[b"itch_book.symbols"] == b"MSFT" and b"itch_book.source_md5" not in meta
    assert meta[b"itch_book.depth"] == b"2"


def test_duplicate_and_padded_table_names_are_written_once(day, tmp_path):
    out = tmp_path / "out"
    assert cli.main(["convert", day, str(out), "--tables", "bbo, bbo ,trades,trades", "--no-md5"]) == 0
    assert pq.read_table(out / "bbo.parquet").num_rows == 5
    assert pq.read_table(out / "trades.parquet").num_rows == 4


def test_stale_outputs_are_removed(day, tmp_path):
    out = tmp_path / "out"
    assert cli.main(["convert", day, str(out), "--tables", "messages,depth", "--symbols", "MSFT", "--no-md5"]) == 0
    assert cli.main(["convert", day, str(out), "--no-md5"]) == 0
    assert files(out) == ["bbo.parquet", "symbols.parquet", "system_events.parquet", "trades.parquet"]


def test_bad_arguments_exit_2(day, tmp_path, capsys):
    out = str(tmp_path / "o")
    assert cli.main(["convert", day, out, "--tables", "quotes"]) == 2
    assert cli.main(["convert", day, out, "--rows", "0"]) == 2
    assert cli.main(["convert", day, out, "--depth", "0"]) == 2
    assert cli.main(["convert", day, out, "--date", "2019-13-01"]) == 2
    assert cli.main(["convert", day, out, "--symbols", "TOOLONGNAME"]) == 2
    assert cli.main(["convert", str(tmp_path / "nope.gz"), out]) == 2
    undated = tmp_path / "day.bin"
    undated.write_bytes(stream())
    assert cli.main(["convert", str(undated), out]) == 2
    assert cli.main(["convert", str(undated), out, "--date", "2019-12-30", "--no-md5"]) == 0
    err = capsys.readouterr().err
    assert "Traceback" not in err and "error:" in err


def test_failed_conversion_leaves_no_parquet_behind(tmp_path):
    torn = tmp_path / "12302019.NASDAQ_ITCH50.gz"
    torn.write_bytes(gzip.compress(stream())[:-40])
    out = tmp_path / "out"
    assert cli.main(["convert", str(torn), str(out), "--no-md5"]) == 2
    assert files(out) == []


def test_verify_pass_and_fail(day, tmp_path, capsys):
    assert cli.main(["verify", day]) == 0
    assert capsys.readouterr().out.rstrip().endswith("PASS")
    bad = tmp_path / "12302019.NASDAQ_ITCH50.bad"
    bad.write_bytes(stream(missing_ref=True))
    assert cli.main(["verify", str(bad), "--date", "2019-12-30"]) == 1
    assert re.search(r"^missing_ref\s+1$", capsys.readouterr().out, re.M)
    truncated = tmp_path / "12302019.NASDAQ_ITCH50"
    truncated.write_bytes(stream()[:-7])
    assert cli.main(["verify", str(truncated)]) == 2
    assert "truncated" in capsys.readouterr().err
    undated = tmp_path / "day.bin"
    undated.write_bytes(stream())
    assert cli.main(["verify", str(undated)]) == 0
    assert "1970-01-01" in capsys.readouterr().err


LISTING_HTML = (
    '<pre><A HREF="/ITCH/">[To Parent Directory]</A><br><br>'
    ' 7/31/2019 12:16 AM    391242214 <A HREF="/ITCH/Nasdaq%20BX%20ITCH/20190730.BX_ITCH_50.gz">20190730.BX_ITCH_50.gz</A><br>'
    ' 7/31/2019 12:16 AM           70 <A HREF="/ITCH/Nasdaq%20BX%20ITCH/20190730.BX_ITCH_50.gz.md5sum">20190730.BX_ITCH_50.gz.md5sum</A><br>'
    ' 4/29/2020  2:53 PM        &lt;dir&gt; <A HREF="/ITCH/Nasdaq%20BX%20ITCH/March%2020/">March 20</A><br>'
    '12/31/2019  1:14 AM            0 <A HREF="/ITCH/Nasdaq%20ITCH/S121225-v50.txt.gz.done">S121225-v50.txt.gz.done</A><br></pre>'
)


def test_parse_listing_keeps_sizes_and_marks_directories():
    rows = cli.parse_listing(LISTING_HTML)
    assert rows[0] == ("20190730.BX_ITCH_50.gz", 391242214, "/ITCH/Nasdaq%20BX%20ITCH/20190730.BX_ITCH_50.gz")
    assert rows[1][0].endswith(".md5sum") and rows[1][1] == 70
    assert rows[2] == ("March 20", None, "/ITCH/Nasdaq%20BX%20ITCH/March%2020/")
    assert rows[3][1] == 0


def test_list_skips_checksums_markers_and_directories(monkeypatch, capsys):
    class Response(io.BytesIO):
        def __enter__(self):
            return self

        def __exit__(self, *exc):
            pass

    monkeypatch.setattr(cli, "http", lambda url, method="GET", headers=None: Response(LISTING_HTML.encode()))
    assert cli.main(["list"]) == 0
    out = capsys.readouterr().out
    assert re.search(r"20190730\.BX_ITCH_50\.gz\s+0\.39 GB\s+2019-07-30", out)
    assert ".md5sum" not in out and ".done" not in out and "March 20" not in out


PAYLOAD = bytes(range(256)) * 40
NAME = "20190730.BX_ITCH_50.gz"


class RangeHandler(http.server.BaseHTTPRequestHandler):
    md5_body: bytes | None = hashlib.md5(PAYLOAD).hexdigest().encode() + b" *" + NAME.encode()
    honour_range = True
    requests: list[tuple[str, str | None]] = []

    def log_message(self, *a):
        pass

    def do_HEAD(self):
        if self.path == f"/d/{NAME}":
            self.send_response(200)
            self.send_header("Content-Length", str(len(PAYLOAD)))
            self.end_headers()
        else:
            self.send_error(404)

    def do_GET(self):
        self.requests.append((self.path, self.headers.get("Range")))
        if self.path == f"/d/{NAME}.md5sum":
            if self.md5_body is None:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Length", str(len(self.md5_body)))
            self.end_headers()
            self.wfile.write(self.md5_body)
            return
        if self.path != f"/d/{NAME}":
            self.send_error(404)
            return
        rng = self.headers.get("Range")
        start = 0
        if rng and self.honour_range:
            start = int(rng.split("=")[1].rstrip("-"))
            if start >= len(PAYLOAD):
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{len(PAYLOAD)}")
                self.end_headers()
                return
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{len(PAYLOAD) - 1}/{len(PAYLOAD)}")
        else:
            self.send_response(200)
        body = PAYLOAD[start:]
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


@pytest.fixture
def emi(monkeypatch):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), RangeHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    RangeHandler.requests = []
    RangeHandler.honour_range = True
    RangeHandler.md5_body = hashlib.md5(PAYLOAD).hexdigest().encode() + b" *" + NAME.encode()
    monkeypatch.setattr(cli, "EMI", f"http://127.0.0.1:{server.server_port}/")
    monkeypatch.setattr(cli, "EMI_DIRS", ("d/",))
    monkeypatch.setattr(cli.time, "sleep", lambda s: None)
    yield RangeHandler
    server.shutdown()


def test_fetch_fresh_and_verified(emi, tmp_path, capsys):
    assert cli.main(["fetch", NAME, "--dir", str(tmp_path)]) == 0
    assert (tmp_path / NAME).read_bytes() == PAYLOAD
    assert "md5 matches" in capsys.readouterr().out
    assert cli.main(["fetch", NAME, "--dir", str(tmp_path)]) == 0
    assert "already exists" in capsys.readouterr().out


def test_fetch_resumes_a_partial_download(emi, tmp_path):
    (tmp_path / (NAME + ".part")).write_bytes(PAYLOAD[:1000])
    assert cli.main(["fetch", NAME, "--dir", str(tmp_path)]) == 0
    assert (tmp_path / NAME).read_bytes() == PAYLOAD
    assert ("/d/" + NAME, "bytes=1000-") in emi.requests


def test_fetch_restarts_when_the_server_ignores_range(emi, tmp_path):
    emi.honour_range = False
    (tmp_path / (NAME + ".part")).write_bytes(b"garbage" * 100)
    assert cli.main(["fetch", NAME, "--dir", str(tmp_path)]) == 0
    assert (tmp_path / NAME).read_bytes() == PAYLOAD


def test_fetch_finishes_a_complete_or_oversized_part_without_416_loops(emi, tmp_path):
    (tmp_path / (NAME + ".part")).write_bytes(PAYLOAD)
    assert cli.main(["fetch", NAME, "--dir", str(tmp_path)]) == 0
    assert all(rng is None for path, rng in emi.requests if path == "/d/" + NAME)
    (tmp_path / NAME).unlink()
    (tmp_path / (NAME + ".part")).write_bytes(PAYLOAD + b"extra")
    assert cli.main(["fetch", NAME, "--dir", str(tmp_path)]) == 0
    assert (tmp_path / NAME).read_bytes() == PAYLOAD


def test_fetch_md5_missing_and_mismatch(emi, tmp_path, capsys):
    emi.md5_body = None
    assert cli.main(["fetch", NAME, "--dir", str(tmp_path)]) == 0
    assert "no checksum published" in capsys.readouterr().out
    (tmp_path / NAME).unlink()
    emi.md5_body = b"0" * 32
    assert cli.main(["fetch", NAME, "--dir", str(tmp_path)]) == 1
    assert not (tmp_path / NAME).exists() and (tmp_path / (NAME + ".bad")).exists()
    assert "MISMATCH" in capsys.readouterr().out


def test_fetch_unknown_name(emi, tmp_path, capsys):
    assert cli.main(["fetch", "nope.gz", "--dir", str(tmp_path)]) == 1
    assert "not in any" in capsys.readouterr().err
