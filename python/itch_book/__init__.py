import builtins
import datetime as dt
import gzip
import queue
import threading
from importlib.metadata import version
from types import SimpleNamespace
from typing import Iterator

import numpy as np

from ._core import Session
from ._dates import midnight_ns, session_date

__all__ = ["Feed", "to_polars"]
__version__ = version("itch-book")

PRICE_SCALE = 10_000
TABLES = ("bbo", "symbols")
SYMBOL_COLUMNS = (
    "locate", "symbol", "market_category", "financial_status", "round_lot_size",
    "round_lots_only", "issue_classification", "issue_subtype", "authenticity",
    "short_sale_threshold", "ipo_flag", "luld_tier", "etp_flag", "etp_leverage", "inverse",
)
BBO_COLUMNS = ("ts_event", "seq", "locate", "bid_px", "bid_sz", "bid_ct", "ask_px", "ask_sz", "ask_ct")


def to_polars(table: dict):
    import polars as pl

    df = pl.DataFrame(table)
    if "ts_event" in df.columns:
        df = df.with_columns(pl.col("ts_event").cast(pl.Datetime("ns", "UTC")))
    return df


def _chunks(path: str, chunk_bytes: int) -> Iterator[bytes]:
    with builtins.open(path, "rb") as f:
        gzipped = f.read(2) == b"\x1f\x8b"
        f.seek(0)
        if gzipped:
            with gzip.GzipFile(fileobj=f) as g:
                while chunk := g.read(chunk_bytes):
                    yield chunk
        else:
            while chunk := f.read(chunk_bytes):
                yield chunk


def _put(q: queue.Queue, item, stop: threading.Event) -> bool:
    while not stop.is_set():
        try:
            q.put(item, timeout=0.1)
            return True
        except queue.Full:
            pass
    return False


def _reader(path: str, chunk_bytes: int, q: queue.Queue, stop: threading.Event) -> None:
    try:
        for chunk in _chunks(path, chunk_bytes):
            if not _put(q, chunk, stop):
                return
        _put(q, None, stop)
    except BaseException as e:  # noqa: BLE001
        _put(q, e, stop)


class Feed:
    def __init__(self, path: str, *, date: dt.date | None = None, price_type: str = "float",
                 chunk_bytes: int = 8 << 20):
        if price_type not in ("float", "fixed"):
            raise ValueError("price_type must be 'float' or 'fixed'")
        day = date or session_date(path)
        if day is None:
            raise ValueError(f"cannot infer the session date from {path!r}; pass date=datetime.date(...)")
        self.path = path
        self.date = day
        self.price_type = price_type
        self.chunk_bytes = chunk_bytes
        self.stats: dict | None = None
        self._midnight = np.uint64(midnight_ns(day))
        self._stop = threading.Event()
        self._queue: queue.Queue = queue.Queue()
        self._thread: threading.Thread | None = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self) -> None:
        self._stop.set()
        t = self._thread
        if t is None:
            return
        while t.is_alive():
            try:
                self._queue.get(timeout=0.05)
            except queue.Empty:
                pass
        t.join()
        self._thread = None

    def batches(self, tables: tuple[str, ...] = ("bbo",), rows: int = 1_000_000) -> Iterator[SimpleNamespace]:
        unknown = set(tables) - set(TABLES)
        if unknown:
            raise ValueError(f"unknown tables {sorted(unknown)}; available: {TABLES}")
        session = Session()
        session.reserve(rows)
        self._stop = threading.Event()
        self._queue = queue.Queue(maxsize=4)
        self._thread = threading.Thread(
            target=_reader, args=(self.path, self.chunk_bytes, self._queue, self._stop), daemon=True
        )
        self._thread.start()
        try:
            while (item := self._queue.get()) is not None:
                if isinstance(item, BaseException):
                    raise item
                session.feed(item)
                if "bbo" in tables and session.bbo_rows() >= rows:
                    yield self._batch(session, tables)
            stats = session.stats()
            if stats["messages"] == 0 or stats["pending_bytes"]:
                raise ValueError(
                    f"{self.path}: not an ITCH 5.0 stream "
                    f"({stats['messages']} messages decoded, {stats['pending_bytes']} trailing bytes)"
                )
            self.stats = stats
            yield self._batch(session, tables)
        finally:
            self.close()

    def _batch(self, session: Session, tables: tuple[str, ...]) -> SimpleNamespace:
        out = {}
        if "bbo" in tables:
            out["bbo"] = self._bbo(session.take_bbo())
        if "symbols" in tables:
            rows = session.take_symbols()
            out["symbols"] = {c: [r[i] for r in rows] for i, c in enumerate(SYMBOL_COLUMNS)}
        return SimpleNamespace(**out)

    def _bbo(self, cols: dict[str, np.ndarray]) -> dict[str, np.ndarray]:
        ts = cols.pop("ts")
        ts += self._midnight
        cols["ts_event"] = ts.view(np.int64)
        if self.price_type == "float":
            for side in ("bid", "ask"):
                px = cols[f"{side}_px"]
                f = px / PRICE_SCALE
                f[px == 0] = np.nan
                cols[f"{side}_px"] = f
        return {k: cols[k] for k in BBO_COLUMNS}


def open(path: str, **kwargs) -> Feed:  # noqa: A001
    return Feed(path, **kwargs)
