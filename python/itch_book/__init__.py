import builtins
import datetime as dt
import gzip
import queue
import threading
import warnings
from collections.abc import Iterator
from importlib.metadata import version
from types import SimpleNamespace

import numpy as np

from ._core import Session
from ._dates import midnight_ns, session_date

__all__ = ["Feed", "to_polars"]
__version__ = version("itch-book")

PRICE_SCALE = 10_000
TABLES = ("bbo", "trades", "messages", "depth", "noii", "halts", "reg_sho", "luld", "symbols", "system_events")
CORE_TABLES = ("bbo", "trades", "messages", "noii", "halts", "reg_sho", "luld")
SYMBOL_COLUMNS = (
    "locate", "symbol", "market_category", "financial_status", "round_lot_size",
    "round_lots_only", "issue_classification", "issue_subtype", "authenticity",
    "short_sale_threshold", "ipo_flag", "luld_tier", "etp_flag", "etp_leverage", "inverse",
)
BBO_COLUMNS = ("ts_event", "seq", "locate", "bid_px", "bid_sz", "bid_ct", "ask_px", "ask_sz", "ask_ct")
TRADE_COLUMNS = ("ts_event", "seq", "locate", "kind", "price", "size", "side", "order_id", "match_number", "cross_type")
MESSAGE_COLUMNS = ("ts_event", "seq", "locate", "type", "action", "side", "price", "size", "remaining",
                   "printable", "order_id", "old_order_id", "mpid")
EVENT_COLUMNS = ("ts_event", "seq", "event")
NOII_COLUMNS = ("ts_event", "seq", "locate", "paired", "imbalance", "direction", "far_px", "near_px", "ref_px",
                "cross_type", "variation")
HALT_COLUMNS = ("ts_event", "seq", "locate", "kind", "state", "reason", "market")
REG_SHO_COLUMNS = ("ts_event", "seq", "locate", "action")
LULD_COLUMNS = ("ts_event", "seq", "locate", "ref_px", "upper_px", "lower_px", "extension")
CHAR_COLUMNS = ("kind", "side", "cross_type", "type", "action", "event", "direction", "variation", "state", "market")
SMALLEST_FRAME = 21


def to_polars(table: dict):
    import polars as pl

    df = pl.DataFrame(table)
    casts = [pl.col(c).cast(pl.Utf8) for c in df.columns if df[c].dtype == pl.Binary]
    if "ts_event" in df.columns:
        casts.append(pl.col("ts_event").cast(pl.Datetime("ns", "UTC")))
    return df.with_columns(casts) if casts else df


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
        self.mpids: list[str] = [""]
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

    def batches(self, tables: tuple[str, ...] = ("bbo",), rows: int = 1_000_000,
                depth: int = 10, symbols: str | tuple[str, ...] = ()) -> Iterator[SimpleNamespace]:
        self.close()
        unknown = set(tables) - set(TABLES)
        if unknown:
            raise ValueError(f"unknown tables {sorted(unknown)}; available: {TABLES}")
        if not 1 <= depth <= 50:
            raise ValueError("depth must be between 1 and 50")
        if isinstance(symbols, str):
            symbols = (symbols,)
        wanted = sorted({s.upper() for s in symbols})
        if any(len(s) > 8 for s in wanted):
            raise ValueError("ITCH symbols are at most 8 characters")
        session = Session(tables=[t for t in CORE_TABLES if t in tables],
                          depth=depth if "depth" in tables else 0, symbols=wanted)
        session.reserve(rows + self.chunk_bytes // SMALLEST_FRAME)
        self._stop = threading.Event()
        self._queue = queue.Queue(maxsize=4)
        self._thread = threading.Thread(
            target=_reader, args=(self.path, self.chunk_bytes, self._queue, self._stop), daemon=True
        )
        self._thread.start()
        try:
            while (item := self._next()) is not None:
                if isinstance(item, BaseException):
                    raise item
                session.feed(item)
                if session.rows() >= rows:
                    yield self._batch(session, tables)
            stats = session.stats()
            if stats["messages"] == 0 or stats["pending_bytes"]:
                what = "truncated" if stats["messages"] else "not an ITCH 5.0 stream"
                raise ValueError(
                    f"{self.path}: {what} ({stats['messages']} messages decoded, "
                    f"{stats['pending_bytes']} trailing bytes)"
                )
            if wanted and stats["selected"] < len(wanted):
                warnings.warn(
                    f"{len(wanted) - stats['selected']} of {len(wanted)} symbols not in the stock directory of {self.path}",
                    stacklevel=2,
                )
            self.stats = stats
            yield self._batch(session, tables)
        finally:
            self.close()

    def _next(self):
        while True:
            try:
                return self._queue.get(timeout=1.0)
            except queue.Empty:
                continue

    def _batch(self, session: Session, tables: tuple[str, ...]) -> SimpleNamespace:
        out = {}
        if "bbo" in tables:
            cols = self._common(session.take_bbo())
            self._price(cols, "bid_px")
            self._price(cols, "ask_px")
            out["bbo"] = {k: cols[k] for k in BBO_COLUMNS}
        if "trades" in tables:
            cols = self._common(session.take_trades())
            cols["price"] = cols.pop("px")
            cols["match_number"] = cols.pop("match")
            self._price(cols, "price")
            out["trades"] = {k: cols[k] for k in TRADE_COLUMNS}
        if "messages" in tables:
            cols = self._common(session.take_messages())
            cols["price"] = cols.pop("px")
            cols["printable"] = cols["printable"].view(np.bool_)
            self._price(cols, "price")
            self.mpids = session.mpids()
            out["messages"] = {k: cols[k] for k in MESSAGE_COLUMNS}
        if "depth" in tables:
            cols = self._common(session.take_depth())
            for name in [c for c in cols if "_px_" in c]:
                self._price(cols, name)
            head = ("ts_event", "seq", "locate")
            out["depth"] = {k: cols[k] for k in head} | {k: v for k, v in cols.items() if k not in head}
        if "noii" in tables:
            cols = self._common(session.take_noii())
            for name in ("far_px", "near_px", "ref_px"):
                self._price(cols, name)
            out["noii"] = {k: cols[k] for k in NOII_COLUMNS}
        if "halts" in tables:
            cols = self._common(session.take_halts())
            cols["reason"] = cols["reason"].view("S4")
            out["halts"] = {k: cols[k] for k in HALT_COLUMNS}
        if "reg_sho" in tables:
            cols = self._common(session.take_reg_sho())
            out["reg_sho"] = {k: cols[k] for k in REG_SHO_COLUMNS}
        if "luld" in tables:
            cols = self._common(session.take_luld())
            for name in ("ref_px", "upper_px", "lower_px"):
                self._price(cols, name)
            out["luld"] = {k: cols[k] for k in LULD_COLUMNS}
        if "system_events" in tables:
            cols = self._common(session.take_events())
            out["system_events"] = {k: cols[k] for k in EVENT_COLUMNS}
        if "symbols" in tables:
            rows = session.take_symbols()
            out["symbols"] = {c: [r[i] for r in rows] for i, c in enumerate(SYMBOL_COLUMNS)}
        return SimpleNamespace(**out)

    def _common(self, cols: dict[str, np.ndarray]) -> dict[str, np.ndarray]:
        ts = cols.pop("ts")
        ts += self._midnight
        cols["ts_event"] = ts.view(np.int64)
        for c in CHAR_COLUMNS:
            if c in cols:
                cols[c] = cols[c].view("S1")
        return cols

    def _price(self, cols: dict[str, np.ndarray], name: str) -> None:
        if self.price_type == "float":
            px = cols[name]
            cols[name] = np.where(px == 0, np.nan, px / PRICE_SCALE)


def open(path: str, **kwargs) -> Feed:  # noqa: A001
    return Feed(path, **kwargs)
