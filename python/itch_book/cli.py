import argparse
import datetime as dt
import hashlib
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import warnings
from pathlib import Path

import numpy as np

from . import PRICE_SCALE, TABLES, Feed, __version__
from ._dates import session_date

EMI = "https://emi.nasdaq.com/ITCH/"
EMI_DIRS = ("Nasdaq ITCH/", "Nasdaq BX ITCH/", "Nasdaq PSX ITCH/")
ROW_TABLES = ("bbo", "trades", "messages", "depth")
DEFAULT_TABLES = ("bbo", "trades")
SCHEMA_VERSION = "1"
CHUNK = 8 << 20
ATTEMPTS = 5
LISTING = re.compile(
    r'(\d{1,2}/\d{1,2}/\d{4})\s+\d{1,2}:\d{2} [AP]M\s+(\d+|&lt;dir&gt;)\s+<A HREF="([^"]+)">([^<]+)</A>',
    re.IGNORECASE,
)


def eprint(*a):
    print(*a, file=sys.stderr, flush=True)


def file_md5(path: str) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        while chunk := f.read(CHUNK):
            h.update(chunk)
    return h.hexdigest()


def strings(arr: np.ndarray):
    import pyarrow as pa

    return pa.Array.from_buffers(pa.binary(1), len(arr), [None, pa.py_buffer(arr)]).cast(pa.string())


class Names:
    def __init__(self):
        self.lookup = np.full(1, "", dtype="U8")
        self.rows: dict[str, list] = {}

    def _grow(self, top: int) -> None:
        if top >= len(self.lookup):
            grown = np.full(top + 1, "", dtype="U8")
            grown[: len(self.lookup)] = self.lookup
            self.lookup = grown

    def update(self, symbols: dict[str, list]) -> None:
        if not symbols["locate"]:
            return
        self._grow(max(symbols["locate"]))
        for loc, name in zip(symbols["locate"], symbols["symbol"]):
            self.lookup[loc] = name
        for k, v in symbols.items():
            self.rows.setdefault(k, []).extend(v)

    def column(self, locate: np.ndarray):
        import pyarrow as pa

        if len(locate):
            self._grow(int(locate.max()))
        return pa.DictionaryArray.from_arrays(pa.array(locate.astype(np.int32)), pa.array(self.lookup))

    def table(self):
        import pyarrow as pa

        t = pa.table(self.rows)
        for name, typ in (("locate", pa.uint16()), ("round_lot_size", pa.uint32()), ("etp_leverage", pa.uint32())):
            i = t.schema.get_field_index(name)
            t = t.set_column(i, name, t.column(i).cast(typ))
        return t


def to_arrow(table: dict[str, np.ndarray], names: Names):
    import pyarrow as pa

    cols = {}
    for name, arr in table.items():
        if name == "ts_event":
            cols[name] = pa.array(arr).cast(pa.timestamp("ns", tz="UTC"))
        elif arr.dtype.kind == "S":
            cols[name] = strings(arr)
        else:
            cols[name] = pa.array(arr)
        if name == "locate":
            cols["symbol"] = names.column(arr)
    return pa.table(cols)


class Writers:
    def __init__(self, out: Path):
        self.out = out
        self.writers = {}
        self.rows: dict[str, int] = {}

    def part(self, table: str) -> Path:
        return self.out / f"{table}.parquet.part"

    def final(self, table: str) -> Path:
        return self.out / f"{table}.parquet"

    def write(self, table: str, arrow) -> None:
        import pyarrow.parquet as pq

        w = self.writers.get(table)
        if w is None:
            names = arrow.schema.names
            sorting = [pq.SortingColumn(names.index(c)) for c in ("ts_event", "seq") if c in names]
            w = pq.ParquetWriter(self.part(table), arrow.schema, compression="zstd", sorting_columns=sorting or None)
            self.writers[table] = w
            self.rows[table] = 0
        if arrow.num_rows:
            w.write_table(arrow, row_group_size=arrow.num_rows)
            self.rows[table] += arrow.num_rows

    def close(self, metadata: dict[str, str]) -> None:
        for table, w in self.writers.items():
            w.add_key_value_metadata(metadata)
            w.close()
            os.replace(self.part(table), self.final(table))
        self.writers.clear()

    def abort(self) -> None:
        for table, w in self.writers.items():
            try:
                w.close()
            finally:
                self.part(table).unlink(missing_ok=True)
        self.writers.clear()


def concat(parts: list[dict[str, np.ndarray]]) -> dict[str, np.ndarray]:
    return {k: np.concatenate([p[k] for p in parts]) for k in parts[0]}


def split(spec: str | None, default: tuple[str, ...], upper: bool = False) -> tuple[str, ...]:
    if not spec:
        return default
    names = (s.strip() for s in spec.split(","))
    return tuple(dict.fromkeys(s.upper() if upper else s for s in names if s))


def convert(args) -> int:
    try:
        import pyarrow  # noqa: F401
    except ImportError:
        eprint("itch2parquet convert needs pyarrow: pip install 'itch-book[cli]'")
        return 2
    tables = split(args.tables, DEFAULT_TABLES)
    bad = set(tables) - set(ROW_TABLES)
    if bad:
        eprint(f"unknown tables {sorted(bad)}; choose from {','.join(ROW_TABLES)}")
        return 2
    if args.rows < 1:
        eprint("--rows must be at least 1")
        return 2
    symbols = split(args.symbols, (), upper=True)
    if "messages" in tables and not symbols:
        eprint("messages for every symbol is the whole order flow (hundreds of millions of rows on a NASDAQ day); --symbols narrows it")
    if args.date:
        date = dt.date.fromisoformat(args.date)
    else:
        date = session_date(args.file)
        if date is None:
            eprint(f"cannot infer the session date from {args.file!r}; pass --date YYYY-MM-DD")
            return 2
        eprint(f"session date {date} inferred from the filename (--date to override)")

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for t in TABLES:
        (out / f"{t}.parquet").unlink(missing_ok=True)
        (out / f"{t}.parquet.part").unlink(missing_ok=True)

    t0 = time.perf_counter()
    md5 = None if args.no_md5 else file_md5(args.file)
    feed = Feed(args.file, date=date, price_type=args.price_type)
    names = Names()
    writers = Writers(out)
    trades_parts: list[dict] = []
    events_parts: list[dict] = []
    try:
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")
            for batch in feed.batches(tables=tables + ("symbols", "system_events"), rows=args.rows,
                                      depth=args.depth, symbols=symbols):
                names.update(batch.symbols)
                events_parts.append(batch.system_events)
                for t in tables:
                    if t == "trades":
                        trades_parts.append(batch.trades)
                    else:
                        writers.write(t, to_arrow(getattr(batch, t), names))
        for w in caught:
            eprint(w.message)
        if "trades" in tables:
            trades = concat(trades_parts)
            voided = trades["match_number"][trades["kind"] == b"B"]
            trades["broken"] = np.isin(trades["match_number"], voided) & (trades["kind"] != b"B")
            n = len(trades["seq"])
            for start in range(0, max(n, 1), args.rows):
                writers.write("trades", to_arrow({k: v[start:start + args.rows] for k, v in trades.items()}, names))
        writers.write("system_events", to_arrow(concat(events_parts), names))
        if names.rows:
            writers.write("symbols", names.table())
        stats = feed.stats
        metadata = {
            "itch_book.schema": SCHEMA_VERSION,
            "itch_book.version": __version__,
            "itch_book.created": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
            "itch_book.spec": "NASDAQ TotalView-ITCH 5.0",
            "itch_book.source": Path(args.file).name,
            "itch_book.session_date": date.isoformat(),
            "itch_book.tz": "America/New_York",
            "itch_book.price_type": args.price_type,
            "itch_book.price_scale": str(PRICE_SCALE if args.price_type == "fixed" else 1),
            "itch_book.tables": ",".join(tables),
            "itch_book.symbols": ",".join(symbols) if symbols else "all",
            "itch_book.stats": json.dumps(stats),
        }
        if md5:
            metadata["itch_book.source_md5"] = md5
        if "depth" in tables:
            metadata["itch_book.depth"] = str(args.depth)
        writers.close(metadata)
    except BaseException:
        writers.abort()
        raise
    elapsed = time.perf_counter() - t0
    for t, n in writers.rows.items():
        size = writers.final(t).stat().st_size
        print(f"{t:14s} {n:>12,} rows {size / 1e6:>9.1f} MB  {writers.final(t)}")
    print(f"{stats['messages']:,} messages in {elapsed:.1f}s; missing_ref={stats['missing_ref']} "
          f"crossed_books={stats['crossed_books']} last_event={stats['last_event']}")
    return 0


def verify(args) -> int:
    date = dt.date.fromisoformat(args.date) if args.date else session_date(args.file)
    if date is None:
        eprint("session date not in the filename; counting with 1970-01-01 (--date to set it)")
        date = dt.date(1970, 1, 1)
    feed = Feed(args.file, date=date)
    for _ in feed.batches(tables=("system_events",), rows=1 << 30):
        pass
    s = feed.stats
    for k in ("messages", "unknown", "malformed", "pending_bytes", "adds", "executes", "cancels", "deletes",
              "replaces", "rejected", "dup_ref", "clamped", "missing_ref", "books", "live_orders",
              "crossed_books", "last_event", "end_of_session"):
        print(f"{k:16s} {s[k]}")
    clean = (s["unknown"] == 0 and s["malformed"] == 0 and s["pending_bytes"] == 0 and s["missing_ref"] == 0
             and s["crossed_books"] == 0 and (s["last_event"] == "C" or s["end_of_session"]))
    print("PASS" if clean else "FAIL")
    return 0 if clean else 1


def parse_listing(html: str) -> list[tuple[str, int | None, str]]:
    return [(name, None if size.startswith("&lt;") else int(size), href)
            for _date, size, href, name in LISTING.findall(html)]


def http(url: str, method: str = "GET", headers: dict | None = None):
    req = urllib.request.Request(url, method=method, headers={"User-Agent": f"itch-book/{__version__}", **(headers or {})})
    return urllib.request.urlopen(req, timeout=60)


def list_files(args) -> int:
    for d in EMI_DIRS:
        url = EMI + urllib.parse.quote(d)
        try:
            with http(url) as r:
                html = r.read().decode("latin-1")
        except (urllib.error.URLError, OSError) as e:
            eprint(f"{d}: {e}")
            continue
        print(d.rstrip("/"))
        for name, size, _href in parse_listing(html):
            if size is None or name.endswith((".md5sum", ".done")):
                continue
            print(f"  {name:36s} {size / 1e9:6.2f} GB  {session_date(name) or '-'}")
    return 0


def locate_remote(name: str) -> tuple[str, int] | None:
    for d in EMI_DIRS:
        url = EMI + urllib.parse.quote(d) + urllib.parse.quote(name)
        try:
            with http(url, "HEAD") as r:
                return url, int(r.headers.get("Content-Length", "0"))
        except urllib.error.HTTPError as e:
            if e.code != 404:
                eprint(f"{url}: HTTP {e.code}")
                return None
        except (urllib.error.URLError, OSError) as e:
            eprint(f"{url}: {e}")
            return None
    eprint(f"{name} is not in any emi.nasdaq.com ITCH directory")
    return None


def download(url: str, part: Path, have: int, total: int) -> None:
    headers = {"Range": f"bytes={have}-"} if have else {}
    with http(url, headers=headers) as r:
        if have and r.status != 206:
            have = 0
        with open(part, "ab" if have else "wb") as f:
            done = have
            last = time.monotonic()
            while chunk := r.read(CHUNK):
                f.write(chunk)
                done += len(chunk)
                if time.monotonic() - last > 2:
                    eprint(f"  {done / 1e9:.2f} / {total / 1e9:.2f} GB" if total else f"  {done / 1e9:.2f} GB")
                    last = time.monotonic()


def fetch(args) -> int:
    dest = Path(args.dir) / args.name
    if dest.exists():
        print(f"{dest} already exists ({dest.stat().st_size:,} bytes); delete it to download again")
        return 0
    found = locate_remote(args.name)
    if found is None:
        return 1
    url, total = found
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    for attempt in range(1, ATTEMPTS + 1):
        have = part.stat().st_size if part.exists() else 0
        if total and have > total:
            part.unlink()
            have = 0
        if total and have == total:
            break
        try:
            download(url, part, have, total)
            break
        except (urllib.error.URLError, OSError) as e:
            eprint(f"attempt {attempt}/{ATTEMPTS} failed: {e}")
            if attempt == ATTEMPTS:
                eprint(f"giving up; re-run to resume from {part.stat().st_size if part.exists() else 0} bytes")
                return 1
            time.sleep(2 * attempt)
    digest = file_md5(str(part))
    part.replace(dest)
    print(f"{dest}  {dest.stat().st_size:,} bytes  md5 {digest}")
    try:
        with http(url + ".md5sum") as r:
            published = re.search(r"[0-9a-fA-F]{32}", r.read().decode("latin-1"))
    except (urllib.error.URLError, OSError):
        published = None
    if published is None:
        print("no checksum published for this file")
        return 0
    if published.group(0).lower() == digest:
        print("md5 matches the published checksum")
        return 0
    bad = dest.with_name(dest.name + ".bad")
    dest.replace(bad)
    print(f"md5 MISMATCH: published {published.group(0).lower()}; kept the download as {bad}")
    return 1


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="itch2parquet",
                                description="NASDAQ TotalView-ITCH 5.0 day files to Parquet research tables.")
    p.add_argument("--version", action="version", version=f"itch-book {__version__}")
    sub = p.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("convert", help="write bbo/trades/messages/depth tables as Parquet")
    c.add_argument("file", help="ITCH 5.0 day file, raw or gzipped")
    c.add_argument("out", help="output directory (existing table files there are replaced)")
    c.add_argument("--tables", help=f"comma-separated subset of {','.join(ROW_TABLES)} (default bbo,trades)")
    c.add_argument("--symbols", help="comma-separated symbols to emit; every book is still maintained")
    c.add_argument("--depth", type=int, default=10, help="levels per side for the depth table, 1-50 (default 10)")
    c.add_argument("--price-type", choices=("float", "fixed"), default="float",
                   help="float64 dollars, or the raw int64 mantissa (dollars = value / 10000)")
    c.add_argument("--date", help="session date YYYY-MM-DD when the filename does not carry it")
    c.add_argument("--rows", type=int, default=1_000_000,
                   help="approximate rows per Parquet row group; batches are cut on input chunks (default 1e6)")
    c.add_argument("--no-md5", action="store_true", help="skip hashing the input into the metadata")
    c.set_defaults(run=convert)

    v = sub.add_parser("verify", help="replay a file and check the book invariants")
    v.add_argument("file", help="ITCH 5.0 day file, raw or gzipped")
    v.add_argument("--date", help="session date YYYY-MM-DD when the filename does not carry it")
    v.set_defaults(run=verify)

    ls = sub.add_parser("list", help="list the day files on emi.nasdaq.com")
    ls.set_defaults(run=list_files)

    f = sub.add_parser("fetch", help="download a day file from emi.nasdaq.com, resuming and checking md5")
    f.add_argument("name", help="file name as shown by list, e.g. 20190730.BX_ITCH_50.gz")
    f.add_argument("--dir", default=".", help="destination directory (default: current)")
    f.set_defaults(run=fetch)

    args = p.parse_args(argv)
    try:
        return args.run(args)
    except KeyboardInterrupt:
        eprint("interrupted")
        return 130
    except (ValueError, OSError, EOFError) as e:
        eprint(f"error: {e}")
        return 2
