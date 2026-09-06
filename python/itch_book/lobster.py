import datetime as dt
from pathlib import Path

import numpy as np

from . import Feed
from ._dates import midnight_ns

OPEN_MS = 34_200_000
CLOSE_MS = 57_600_000
ASK_EMPTY = 9_999_999_999
BID_EMPTY = -9_999_999_999
HALT_PRICE = {"H": -1, "P": -1, "Q": 0, "T": 1}


def export(path: str, out: str, symbols, levels: int = 10, date: dt.date | None = None) -> list[tuple[str, int]]:
    import pyarrow as pa
    import pyarrow.csv as pcsv

    feed = Feed(path, date=date, price_type="fixed")
    tables = ("messages", "trades", "halts", "depth")
    parts = {t: [] for t in tables}
    names = {}
    for b in feed.batches(tables=tables + ("symbols",), rows=1_000_000, depth=levels, symbols=symbols):
        names.update(zip(b.symbols["locate"], b.symbols["symbol"]))
        for t in tables:
            parts[t].append(getattr(b, t))
    cat = {t: {k: np.concatenate([p[k] for p in ps]) for k in ps[0]} for t, ps in parts.items()}
    midnight = midnight_ns(feed.date)
    lo, hi = midnight + OPEN_MS * 1_000_000, midnight + CLOSE_MS * 1_000_000
    wanted = {s.upper() for s in ((symbols,) if isinstance(symbols, str) else symbols)}
    written = []
    for locate, sym in sorted(names.items()):
        if sym not in wanted:
            continue
        rows = _messages(cat, locate)
        if not rows:
            continue
        seq, ts, typ, oid, size, px, side = (np.array(c, dtype=np.int64) for c in zip(*rows))
        keep = (ts >= lo) & (ts < hi)
        order = np.argsort(seq[keep], kind="stable")
        seq, ts, typ, oid, size, px, side = (a[keep][order] for a in (seq, ts, typ, oid, size, px, side))
        book = _orderbook(cat["depth"], locate, seq, levels)
        stem = f"{sym}_{feed.date.isoformat()}_{OPEN_MS}_{CLOSE_MS}"
        msg_file = Path(out) / f"{stem}_message_{levels}.csv"
        book_file = Path(out) / f"{stem}_orderbook_{levels}.csv"
        opts = pcsv.WriteOptions(include_header=False)
        pcsv.write_csv(pa.table({
            "time": (ts - midnight) / 1e9, "type": typ, "order_id": oid, "size": size, "price": px, "direction": side,
        }), msg_file, opts)
        pcsv.write_csv(pa.table({f"c{i}": book[:, i] for i in range(book.shape[1])}), book_file, opts)
        written.append((str(msg_file), len(seq)))
        written.append((str(book_file), len(seq)))
    return written


def _messages(cat, locate):
    out = []
    orders: dict[int, tuple[int, int, int]] = {}
    m = cat["messages"]
    for i in np.flatnonzero(m["locate"] == locate):
        side = m["side"][i]
        if side == b"N":
            continue
        d = 1 if side == b"B" else -1
        typ, oid, seq, ts = m["type"][i], int(m["order_id"][i]), int(m["seq"][i]), int(m["ts_event"][i])
        size, px, rem = int(m["size"][i]), int(m["price"][i]), int(m["remaining"][i])
        if typ in (b"A", b"F"):
            if rem:
                orders[oid] = (size, px, d)
                out.append((seq, ts, 1, oid, size, px, d))
        elif typ == b"U":
            old = orders.pop(int(m["old_order_id"][i]), None)
            if old:
                out.append((seq, ts, 3, int(m["old_order_id"][i]), old[0], old[1], old[2]))
            if rem:
                orders[oid] = (size, px, d)
                out.append((seq, ts, 1, oid, size, px, d))
        else:
            live = orders.get(oid)
            resting_px = live[1] if live else px
            code = {b"E": 4, b"C": 4, b"X": 2, b"D": 3}[typ]
            out.append((seq, ts, code, oid, size, px if typ == b"C" else resting_px, d))
            if rem and live:
                orders[oid] = (rem, live[1], live[2])
            else:
                orders.pop(oid, None)
    t = cat["trades"]
    for i in np.flatnonzero((t["locate"] == locate) & ((t["kind"] == b"P") | (t["kind"] == b"Q"))):
        code = 5 if t["kind"][i] == b"P" else 6
        out.append((int(t["seq"][i]), int(t["ts_event"][i]), code, 0, int(t["size"][i]), int(t["price"][i]),
                    1 if code == 5 else -1))
    h = cat["halts"]
    for i in np.flatnonzero(h["locate"] == locate):
        state = h["state"][i].decode()
        price = HALT_PRICE.get(state, -1) if h["kind"][i] == b"H" else (-1 if state == "H" else 1)
        out.append((int(h["seq"][i]), int(h["ts_event"][i]), 7, 0, 0, price, -1))
    return out


def _orderbook(depth, locate, seq, levels):
    sel = np.flatnonzero(depth["locate"] == locate)
    if not sel.size:
        return np.tile(np.array([ASK_EMPTY, 0, BID_EMPTY, 0], dtype=np.int64), (len(seq), levels))
    dseq = depth["seq"][sel].astype(np.int64)
    idx = np.searchsorted(dseq, seq, side="right") - 1
    book = np.empty((len(seq), 4 * levels), dtype=np.int64)
    has = idx >= 0
    rows = sel[np.maximum(idx, 0)]
    for lvl in range(levels):
        tag = f"{lvl:02d}"
        ask_px = depth[f"ask_px_{tag}"][rows].astype(np.int64)
        bid_px = depth[f"bid_px_{tag}"][rows].astype(np.int64)
        ask_sz = depth[f"ask_sz_{tag}"][rows].astype(np.int64)
        bid_sz = depth[f"bid_sz_{tag}"][rows].astype(np.int64)
        ask_on = has & (ask_px != 0)
        bid_on = has & (bid_px != 0)
        book[:, 4 * lvl] = np.where(ask_on, ask_px, ASK_EMPTY)
        book[:, 4 * lvl + 1] = np.where(ask_on, ask_sz, 0)
        book[:, 4 * lvl + 2] = np.where(bid_on, bid_px, BID_EMPTY)
        book[:, 4 * lvl + 3] = np.where(bid_on, bid_sz, 0)
    return book
