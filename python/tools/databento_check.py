"""Cross-check itch-book's top of book against Databento XNAS.ITCH mbp-1.

Reproduces the README "Validation" A4 numbers for one day and a few symbols.

    DATABENTO_API_KEY=... python python/tools/databento_check.py 12302019.NASDAQ_ITCH50.gz AAPL,MSFT,SPY

Prints, per symbol: agreement of the top-of-book collapsed to the last state at each distinct
nanosecond, agreement sampled at every Databento event, and the share of the sampled
disagreements that fall on a nanosecond carrying more than one ITCH message.
"""
import argparse
import datetime as dt

import numpy as np
import pandas as pd

import itch_book as ib

PX = ("bid_px_00", "ask_px_00")
SZ = ("bid_sz_00", "ask_sz_00")


def our_depth(path, symbols, date):
    feed = ib.Feed(path, date=date)
    rows = {s: [] for s in symbols}
    names = {}
    for b in feed.batches(tables=("depth", "symbols"), rows=5_000_000, depth=1, symbols=symbols):
        names.update(zip(b.symbols["locate"], b.symbols["symbol"]))
        d = b.depth
        for i in range(len(d["seq"])):
            rows[names[int(d["locate"][i])]].append(
                (int(d["ts_event"][i]), d["bid_px_00"][i], d["bid_sz_00"][i], d["ask_px_00"][i], d["ask_sz_00"][i]))
    return {s: pd.DataFrame(r, columns=["ts", "bid_px_00", "bid_sz_00", "ask_px_00", "ask_sz_00"]) for s, r in rows.items()}


def databento_mbp1(client, symbols, date, max_cost):
    args = dict(dataset="XNAS.ITCH", schema="mbp-1", symbols=list(symbols),
                start=date.isoformat(), end=(date + dt.timedelta(days=1)).isoformat())
    cost = client.metadata.get_cost(**args)
    if cost > max_cost:
        raise SystemExit(f"databento cost ${cost:.2f} above --max-cost {max_cost}")
    df = client.timeseries.get_range(**args).to_df(price_type="float", pretty_ts=False, map_symbols=True).reset_index()
    df["ts"] = df["ts_event"].astype("int64")
    return cost, df


def eq(a, b):
    a, b = np.asarray(a, float), np.asarray(b, float)
    return np.isclose(a, b) | (np.isnan(a) & np.isnan(b))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("file")
    p.add_argument("symbols")
    p.add_argument("--max-cost", type=float, default=1.0)
    args = p.parse_args()
    import databento as db

    symbols = tuple(args.symbols.upper().split(","))
    date = ib.session_date(args.file)
    mine = our_depth(args.file, symbols, date)
    cost, dbf = databento_mbp1(db.Historical(), symbols, date, args.max_cost)
    print(f"XNAS.ITCH mbp-1 {date} {list(symbols)}: {len(dbf):,} records, cost ${cost:.4f}\n")
    print(f"{'sym':6s} {'ns_price':>9s} {'ns_pxsz':>9s} {'evt_price':>10s} {'diffs_multi_msg_ns':>19s}")
    for s in symbols:
        m = mine[s].sort_values("ts")
        d = dbf[dbf.symbol == s].sort_values("ts")
        ml = m.groupby("ts").last()
        dl = d.groupby("ts").last()
        j = dl.join(ml, how="inner", lsuffix="_db", rsuffix="_our")
        ns_px = eq(j.bid_px_00_db, j.bid_px_00_our) & eq(j.ask_px_00_db, j.ask_px_00_our)
        ns_pxsz = ns_px & (j.bid_sz_00_db.to_numpy() == j.bid_sz_00_our.to_numpy()) & (j.ask_sz_00_db.to_numpy() == j.ask_sz_00_our.to_numpy())
        idx = np.searchsorted(m.ts.to_numpy(), d.ts.to_numpy(), side="right") - 1
        ok = idx >= 0
        de = d[ok].reset_index(drop=True)
        me = m.iloc[idx[ok]].reset_index(drop=True)
        evt_px = eq(de.bid_px_00, me.bid_px_00) & eq(de.ask_px_00, me.ask_px_00)
        neq = ~evt_px
        on_tie = neq & (de.ts.to_numpy() == me.ts.to_numpy())
        share = on_tie.sum() / neq.sum() * 100 if neq.sum() else 100.0
        print(f"{s:6s} {ns_px.mean() * 100:>8.3f}% {ns_pxsz.mean() * 100:>8.3f}% {evt_px.mean() * 100:>9.3f}% {share:>18.1f}%")


if __name__ == "__main__":
    main()
