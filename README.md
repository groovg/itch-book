# itch-book

[![CI](https://github.com/groovg/itch-book/actions/workflows/ci.yml/badge.svg)](https://github.com/groovg/itch-book/actions/workflows/ci.yml)
[![PyPI](https://img.shields.io/pypi/v/itch-book)](https://pypi.org/project/itch-book/)
[![Python](https://img.shields.io/pypi/pyversions/itch-book)](https://pypi.org/project/itch-book/)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

NASDAQ TotalView-ITCH 5.0 feed handler and limit order book reconstruction. A header-only
C++23 core (no dependencies beyond [fixed-decimal](https://github.com/groovg/fixed-decimal)
for exact prices) and a Python package on top of it that turns the raw day files NASDAQ
publishes at [emi.nasdaq.com/ITCH](https://emi.nasdaq.com/ITCH/) into BBO, trades,
order-by-order and depth tables, as numpy batches or Parquet.

On a full trading day (`12302019.NASDAQ_ITCH50`, 268.7M messages, 8.25 GB) the C++ core
replays parse + full book apply for all 8,907 symbols at **17.3M messages/s single-threaded**
(58 ns/message) inside **1.4 GB** of book structures, with zero unresolved order references
and zero crossed books at the close. The Python CLI converts the same day, gzipped, to
`bbo` + `trades` Parquet in **62 s**; the resulting book agrees with Databento's XNAS.ITCH
feed on 99.7 to 99.9% of nanoseconds ([validation](#validation)).

## Contents

- [Install](#install)
- [Quick start](#quick-start)
- [Python package](#python-package)
  - [Tables](#tables)
  - [Trades and messages](#trades-and-messages)
  - [Symbols, depth, batches](#symbols-depth-batches)
  - [itch2parquet](#itch2parquet)
  - [Notes and limits](#notes-and-limits)
- [C++ library](#c-library)
  - [Handlers and streaming](#handlers-and-streaming)
  - [Build, tests, tools](#build-tests-tools)
  - [Wire format](#wire-format)
  - [Book design](#book-design)
- [Correctness](#correctness)
- [Validation](#validation)
- [Benchmarks](#benchmarks)
- [Limitations](#limitations)
- [Production notes](#production-notes)
- [License](#license)

## Install

Python 3.10+, wheels for Linux x86_64 and aarch64 (manylinux_2_28), macOS 11+ arm64 and
x86_64, Windows x64. The package depends on numpy and tzdata; a source build needs a C++23
compiler.

```
pip install itch-book            # numpy batches
pip install "itch-book[cli]"     # + itch2parquet (pyarrow)
pip install "itch-book[polars]"  # + to_polars
```

C++: header-only, add `include/` to the include path and link `fixed_decimal` (the root
`CMakeLists.txt` fetches it, or `add_subdirectory` this repo and link the `itch_book`
target). GCC and Clang; MSVC is out because the price type needs `__int128`, on Windows
build with clang-cl.

## Quick start

Python:

```python
import itch_book as ib

feed = ib.open("20190730.BX_ITCH_50.gz")          # session date from the filename
for batch in feed.batches(tables=("bbo", "symbols"), rows=1_000_000):
    df = ib.to_polars(batch.bbo)                  # ts_event as Datetime("ns", "UTC")
feed.stats                                        # message counts and book invariants
```

Command line:

```
itch2parquet list                                   # what emi.nasdaq.com has, with sizes and session dates
itch2parquet fetch 20190730.BX_ITCH_50.gz --dir data # resumes a partial download, checks the published md5
itch2parquet verify data/20190730.BX_ITCH_50.gz     # replays the day and prints the book invariants
itch2parquet convert data/20190730.BX_ITCH_50.gz out # bbo + trades by default
itch2parquet convert FILE out --tables messages,depth --symbols AAPL,MSFT --depth 5 --price-type fixed
itch2parquet lobster FILE out --symbols AAPL,MSFT --levels 10   # LOBSTER message + orderbook csv per symbol
```

C++:

```cpp
#include <itch/book_manager.hpp>
#include <itch/mapped_file.hpp>
#include <itch/parser.hpp>

itch::MappedFile file("12302019.NASDAQ_ITCH50");
itch::BookManager<> books;
itch::ParseResult r = itch::parse(file.bytes(), books);

itch::Bbo q = books.bbo(locate);            // best bid/offer for a symbol
const auto* book = books.book(locate);      // full depth, FIFO queues per level
```

## Python package

The package reads raw or gzipped day files and hands out columnar batches as numpy arrays,
zero-copy, so Polars, pandas and pyarrow ingest them without conversion. Decompression runs
on a reader thread in Python's zlib; the parser and books run in C++ with the GIL released.

### Tables

Every table carries `ts_event` (int64 ns UTC: New York midnight of the session plus the
ITCH timestamp), `seq` (ordinal among decoded messages) and `locate`. Prices are float64
by default (every ITCH `Price(4)` is exact in a double) or the raw int64 mantissa with
`price_type="fixed"`; a missing price is NaN / 0. Single-character columns come out as
`S1`; `to_polars` turns them into strings. `rows` is a lower bound per batch: batches are
cut at chunk boundaries.

| table | one row per | columns |
|---|---|---|
| `bbo` | best bid or offer change (price or size) | `bid_px bid_sz bid_ct ask_px ask_sz ask_ct` |
| `trades` | E, printable C, P, Q, B | `kind price size side order_id match_number cross_type` |
| `messages` | A, F, E, C, X, D, U | `type action side price size remaining printable order_id old_order_id mpid` |
| `depth` | change within the top N levels of either side (`depth=10`) | `bid_px_00 bid_sz_00 bid_ct_00 ask_px_00 … ask_ct_09` |
| `noii` | I (net order imbalance indicator) | `paired imbalance direction far_px near_px ref_px cross_type variation` |
| `halts` | H (trading action) and h (operational halt) | `kind state reason market` |
| `reg_sho` | Y | `action` |
| `luld` | J (LULD auction collar) | `ref_px upper_px lower_px extension` |
| `system_events` | S | `event` |
| `symbols` | R | the stock directory fields |

### Trades and messages

`trades`: an `E` prints at the resting order's price, a `C` at the message price and only
when printable, `side` is the resting side for E/C and `N` otherwise (the P side field is
always `B` on the wire and carries nothing), `order_id` is the resting order for E/C and 0
otherwise, `size` is uint64 because cross sizes are 8 bytes. A `B` row carries only
`match_number`; the trade it voids may sit in an earlier batch, so anti-join over the day.

`messages` is order-by-order with the resting state looked up before the message is
applied: `side`, `locate` and (for E/X/D) `price` come from the resting order, so a D/X/E
row is self-contained; `remaining` is what is left after apply, clamped at zero. `action`
is `A` add, `F` fill (E/C), `C` cancel (X/D), `M` replace (U, where `order_id` is the new
reference and `old_order_id` the old). Rows the book did not apply say so: `side == N` means
the reference was unknown and nothing changed; an A or U with `remaining == 0` was rejected
(zero shares or price). An A or U onto a live reference evicts it first. Non-printable C
executions are here with `printable == False` and absent from `trades`. `mpid` indexes
`feed.mpids` (0 = none) and is only set on F rows. `test_tables.py` replays this table with
those rules and reproduces `bbo` exactly on random days that include unknown references,
over-sized executes and duplicate references.

### Symbols, depth, batches

`batches(..., symbols=("AAPL", "MSFT"))` keeps every book (executes and replaces need the
order they refer to, wherever it lives) and emits rows only for the selected locates;
`feed.stats` stays feed-wide, and `stats["selected"]` says how many names matched the
day's directory (a miss is a warning). `depth` rows carry no trigger columns; join
`messages` on `seq` for the event that produced a snapshot. On the BX day 99.8% of
book-changing messages touch the top ten levels, so `depth` at N=10 is effectively one row
per event there. `symbols` is the stock directory keyed by `locate`.

`noii` is the imbalance feed NASDAQ disseminates every second during the opening and
closing cross windows (and around halts and IPOs): paired and imbalance shares, the far,
near and current reference prices (NaN when not disseminated) and the cross it refers to.
`halts` merges the two halt message types: `kind` is `H` (stock trading action, `state`
T/H/P/Q with the four-letter `reason`) or `h` (operational halt, `state` H/T for the
`market` Q/B/X). `reg_sho` carries the Reg SHO action (0/1/2) and `luld` the LULD auction
collar reference, upper and lower prices with the extension counter. The other
administrative types (L, V, W, K, N, O) are counted in `feed.stats` and not decoded.

BX 2019-07-30 (391 MB gzip, 28.7M messages, 8,849 symbols), gunzip included: `bbo` alone
2.3 s (19.1M rows), the five row tables without `depth` 2.5 s (`messages` 23.8M rows,
`trades` 925k), every book invariant at zero. `depth` at N=10 for all 8,849 symbols is the
one expensive table: 7.0 s for 23.8M rows of 63 columns; with three symbols selected the
whole run is back to 2.3 s.

### itch2parquet

`pip install "itch-book[cli]"` adds a command that writes the tables as Parquet (pyarrow,
zstd, one row group per batch) and takes care of getting the data; the subcommands are in
[Quick start](#quick-start).

Every row table gets a dictionary-encoded `symbol` column next to `locate`, `trades` gets a
`broken` flag (true on a print that a later `B` voided; the `B` rows stay; the BX day above
has no `B` at all, so that path is exercised by the tests only), and `symbols.parquet` /
`system_events.parquet` are always written. Files are written to `.part` and renamed at the
end, so a failed run leaves nothing behind, and stale table files from an earlier run in the
same directory are removed first. Each file records `ts_event` and `seq` as its Parquet
sorting columns; both are non-decreasing over a whole day, verified on the days under
[Validation](#validation).

The footer of each file carries `itch_book.*` key-value metadata: a schema version, the
source file name and its md5, session date, time zone, `price_type` and `price_scale`
(dollars = stored value / scale), the table list and symbol filter, tool version, creation
time, and the full stats dictionary (message counts, unresolved references, `crossed_books`
and `live_orders` as of the end of the file, last system event), so a Parquet file states
where it came from and whether the book that produced it was clean. The session date comes
from the filename (both emi naming schemes) and is printed when inferred; `--date` overrides
it. The BX day above converts to `bbo` (267 MB) + `trades` (15 MB) in 6.6 s including the
md5 pass.

### LOBSTER export

`itch2parquet lobster FILE out --symbols AAPL --levels 10` writes the two-file layout used by
[LOBSTER](https://lobsterdata.com/info/DataStructure.php) and the academic order-book
literature: `AAPL_2019-12-30_34200000_57600000_message_10.csv` (time in seconds after
midnight, event type, order id, size, price ×10000, direction) and the matching
`_orderbook_10.csv` (ask price, ask size, bid price, bid size per level, empty levels as
9999999999 / -9999999999 with size 0), one orderbook row per message row, no headers, regular
session only (09:30 to 16:00). Event types: 1 submission (A/F), 2 partial cancel (X), 3 deletion
(D), 4 execution of a visible order (E and C, at the resting price for E and the message price
for C), 5 execution of a hidden order (P; NASDAQ sends `B` in that message's side field, so
direction is 1), 6 cross trade (Q, direction -1), 7 halt (H and h; price -1 halted or paused, 0
quotation only, 1 trading, direction -1). A replace (U) becomes a deletion of the old order
followed by a submission of the new one, and both rows carry the book state after the whole
replace. Messages with an unknown order reference are left out.

### Notes and limits

- Every table streams; Arrow conversion and the zstd write run on their own thread behind a
  two-batch queue. `broken` is filled in by a second pass over `trades.parquet`, row group by
  row group, and only when the day carried a `B` message at all (the three days under
  Validation carry none).
- Unsigned columns are stored with Parquet unsigned annotations, which polars, pyarrow,
  duckdb and pandas read directly and some older JVM readers do not.
- There is no per-symbol partitioning; filter the tables afterwards.
- One abi3 wheel covers 3.12 and later, 3.10 and 3.11 get their own; Windows builds with
  clang-cl.

## C++ library

### Handlers and streaming

Handlers are plain structs; implement only the callbacks you need (`on_add`, `on_execute`,
`on_execute_price`, `on_cancel`, `on_delete`, `on_replace`, `on_trade`, `on_cross`,
`on_broken`, `on_system_event`, `on_stock_directory`, `on_trading_action`,
`on_operational_halt`, `on_noii`, `on_reg_sho`, `on_luld_collar`, and `on_other(char)` for
everything else). Messages you skip cost one length lookup, nothing is decoded for them:

```cpp
struct Trades {
    void on_trade(const itch::Trade& t) { /* ... */ }
};
Trades h;
itch::parse(file.bytes(), h);
```

Input does not have to be one whole buffer. `StreamParser` reassembles frames that arrive
split across arbitrary chunk boundaries (socket reads, packet payloads); whole frames
inside a chunk are still parsed in place, only a partial tail is ever copied:

```cpp
itch::StreamParser stream(books);
while (read_chunk(buf)) stream.feed(buf);
```

The manager can also emit a time-and-sales stream: `E` executions print at the *resting
order's* price, which only the book knows, plus printable `C`, non-cross trades, crosses
and broken-trade voids, all in feed order:

```cpp
itch::BookManager tape(nullptr, [](const itch::TradePrint& t) { /* ... */ });
```

### Build, tests, tools

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Tools: `itch-replay <file> [--book]` (per-type counts, or full book replay with stats),
`gen-synthetic <out> <messages> [symbols] [seed]` (deterministic test feed),
`parse_throughput` / `book_throughput` / `apply_latency` benches (the last needs
`-DITCH_BENCH_LATENCY=ON`, x86 only).

### Wire format

Every message sits behind a 2-byte big-endian length prefix; a zero length marks end of
session. The parser treats the prefix as authoritative: known types are additionally
checked against their fixed spec length (one table lookup), unknown or mismatched frames
are skipped by length and counted, never parsed. Nasdaq adds message types over the years
(`O`, Direct Listing with Capital Raise, arrived in 2023), and parsers that abort on
unknown bytes die on the first file recorded after their spec revision.

All multi-byte fields are big-endian at odd offsets. Fields are decoded with `memcpy`
into an integer plus `std::byteswap`; at `-O2` GCC and Clang compile that to the same
single `mov + bswap` a `reinterpret_cast` of a packed struct would produce, but without
the unaligned-access UB, so the hot path runs clean under UBSan and works on
strict-alignment targets. The 6-byte timestamps are copied as 6 bytes; the popular trick
of one 8-byte load at offset 5 shifted right reads past the end of the buffer on the
final 12-byte message of a file.

Dispatch is a `switch` on the type byte into a compile-time handler concept
(`if constexpr (requires { h.on_add(...); })`), so decode inlines straight into book
application. No virtual calls anywhere on the hot path.

Prices are ITCH `Price(4)`, 32-bit unsigned with four implied decimals, and land in
`fixed_decimal::Fixed<4, PriceTag, int64_t>`: exact integer mantissa arithmetic, no
floats, `from_raw` costs nothing.

### Book design

- Books live in a flat vector indexed by `stock locate` (the spec defines it as a dense,
  day-scoped array index), so no symbol hashing ever happens per message.
- Price levels per side are a sorted vector with the best level at the back. Ask prices
  are stored negated so both sides share one ascending comparator and the same
  scan-from-back loop. Adds and deletes overwhelmingly hit within a few levels of the
  touch, so the linear scan typically ends in 1–5 comparisons; a deep insert pays an
  O(levels) `memmove`, which the latency table below quantifies.
- Level records (aggregate shares, order count, FIFO head/tail) are pooled per book
  behind 32-bit handles with a LIFO freelist: no allocation per level after warm-up, and
  handles stay valid across vector growth.
- Orders carry their level handle, so executes, cancels, deletes and replaces never
  search the book: one order lookup, one level dereference. Messages that mutate an
  existing order are more than half of a NASDAQ day (deletes alone are ~43%), which is
  why this is the property worth paying for.
- FIFO queues are intrusive doubly-linked chains of order references per level, so queue
  position is reconstructible. That is the part aggregate-only books throw away.
- The order-reference index exploits that ITCH refs are day-unique and near-dense: a
  paged direct index (8,192 refs per page) instead of a hash. The default store keeps
  pages of 32-bit handles into a recycled order pool; a page is freed to a spare list the
  moment its last order dies, so resident memory is bounded by the live window of the ref
  space, not by the day's 118M adds. A cap on the accepted ref space (`kMaxRef`) keeps a
  corrupt or adversarial feed from growing the page table without bound.
- `reserve()` on the manager and the store pre-sizes everything for a strict
  zero-allocation steady state, verified by a test that counts global `operator new`
  calls across 400k messages after warm-up: zero.
- Trading-action state (`H`) is tracked per locate and queryable
  (`trading_state(locate)`), but books are deliberately not gated on it: Nasdaq keeps
  order maintenance flowing during halts, so a handler that stops applying messages on
  `H` resumes with a corrupt book.

Fault handling: unknown refs are counted and ignored, duplicate adds replace the stale
order, over-sized executes clamp, zero-share or zero-price messages are rejected. Each
path is unit-tested and mirrored exactly by the reference implementation used for
differential testing.

## Correctness

- Differential test: a deliberately naive reference book (`std::map` levels,
  `std::unordered_map` orders, ~100 lines) consumes the same synthetic feeds as the fast
  book; full book states (every level, every side, live-order counts) are compared at
  checkpoints. Runs over 3 seeds × 400k messages × all three order-store variants.
- Structural invariants (`Book::validate`): sorted sides, level aggregates equal to the
  sum of their FIFO chain, link consistency, order counts.
- Real-data smoke: the full NASDAQ and BX days replay with zero missing refs, zero
  duplicates, zero rejects, zero clamps, and zero crossed books at the close.
- Fuzzing: a libFuzzer harness drives `parse` + book apply in CI (ASan+UBSan); a
  deterministic mutation test (bit flips + truncations over a synthetic feed) runs in the
  regular suite. The framing layer never reads outside the buffer by construction; decode
  only happens after the length check.
- Python: 71 pytest cases over hand-computed rows for every table, chunk splits down to a
  single byte, truncated and multi-member gzip, the CLI end to end against a local HTTP
  server for `fetch`; the abi3 wheel is audited with `abi3audit` in CI.
- CI: GCC, Clang, ASan+UBSan, fuzz, and the Python wheel on Linux and Windows, all on
  every push; the release matrix builds every wheel on its native runner.

## Validation

Numbers below are the 9950X3D machine under [Benchmarks](#benchmarks), `itch2parquet` from
the installed wheel, each reference driven through its own documented interface.

Book invariants, three days end to end (`itch2parquet verify`), every counter zero:

| day | messages | unresolved refs | crossed at close | last event |
|---|---|---|---|---|
| 2019-12-30 NASDAQ (3.5 GB gz) | 268,744,780 | 0 | 0 | C (end of messages) |
| 2025-11-28 NASDAQ `S*-v50` (4.7 GB gz) | 353,357,889 | 0 | 0 | C |
| 2019-07-30 BX (0.39 GB gz) | 28,734,686 | 0 | 0 | C |

The 2025 day carries message types absent in 2019 (they are counted, not decoded) and still
closes clean, so the framing and the book survive a newer feed.

Self-consistency: replaying the `messages` table through the documented rule set reproduces
the `bbo` table row for row. On 2019-12-30 for AAPL, MSFT and SPY that is 2,170,927 top-of-book
rows, identical. The same replay runs on synthetic days in CI over random feeds that include
unknown references, over-sized executes and duplicate references.

Cross-check against Databento XNAS.ITCH `mbp-1`, same day, same three symbols (2.17M records,
$0.19 of metered data; `python/tools/databento_check.py` reproduces it). Collapsed to the last
state at each distinct nanosecond, the top-of-book prices agree on 99.69% (AAPL), 99.88%
(MSFT) and 99.88% (SPY) of nanoseconds, and including the sizes on 99.2 to 99.8%. Sampling
our book at every one of Databento's events instead drops the price agreement to 92 to 98%.
Every disagreement at either granularity sits on a nanosecond that carries more than one
ITCH message, where the two feeds order the sub-events within the nanosecond differently
and Databento models an ITCH replace as a cancel plus an add; on a nanosecond that carries a
single message the two books never disagree. Documented differences: this package has no
`ts_recv` and no `publisher_id`, and an empty side is `NaN`/0 where Databento uses a
sentinel.

## Benchmarks

Machine: AMD Ryzen 9 9950X3D (Zen 5), Windows 11, single thread, no core isolation.

### Python, end to end

The 2019-12-30 NASDAQ day (3.5 GB gz, 268.7M messages), `bbo` + `trades`:

| stage | wall | rate |
|---|---|---|
| gunzip only (Python zlib) | 19.9 s | 8.25 GB out |
| + parse and apply, no output | 35.4 s | 7.6 M msg/s |
| `itch2parquet convert` (adds Arrow + zstd write, 1.9 GB) | 61.8 s | 4.3 M msg/s |
| same with the input md5 pass | 64.8 s | 4.1 M msg/s |

On the same file and machine: `ml4t/itch-parser` (Rust, writes all 21 message types to
5.79 GB of Parquet, a heavier job than the two tables above) finishes in 106 s
(2.5 M msg/s); MeatPy (pure Python) takes 6.4 min just to read the day's messages and 14 min
to run its documented single-symbol order-book example (0.70 and 0.32 M msg/s).

### C++, parse and apply

GCC 16.1 `-O3`. Input: `12302019.NASDAQ_ITCH50` (268,744,780 messages, 8.25 GB) fully
resident in a RAM buffer, so no IO or page-cache effects in the measured loop. Reproduce
with `parse_throughput <file>` and `book_throughput <file> <variant>`.

Parse only:

| tier | throughput | per message |
|---|---|---|
| framing walk (length-prefix skip) | 747 M msg/s (~23 GB/s) | 1.3 ns |
| full decode, all 10 book-affecting types, checksummed | 194 M msg/s | 5.2 ns |

Parse + apply, whole day, all symbols (best of repeated runs; "structures" is peak RSS
minus the input buffer):

| variant | throughput | per message | structures |
|---|---|---|---|
| **pooled pages + order pool (default)** | **17.3 M msg/s** | **58 ns** | **~1.4 GB** |
| inline paged records | 14.5 M msg/s | 69 ns | ~9.8 GB |
| open-addressing flat hash | 9.7 M msg/s | 103 ns | ~0.3 GB |
| `unordered_map` ref index, same book | 5.7 M msg/s | 174 ns | ~0.3 GB |
| naive book (`std::map` + `unordered_map`) | 3.5 M msg/s | 287 ns | ~0.2 GB |

Where the factors come from. Replacing `std::map` levels with the sorted vector is ~1.6×
(touch-local scans instead of pointer chasing). Replacing the hash ref-index with paged
direct indexing is another ~3×: one arithmetic dereference, no hashing, no probe chains,
no rehash stalls, and near-monotonic refs keep the hot pages cached. The flat hash
(fibonacci hashing, linear probing, backward-shift deletion) isolates how much of the
`unordered_map` cost is the container itself: dropping per-node allocation and
bucket-chain chasing buys ~1.7×, but it still hashes, probes and moves 40-byte slots on
every delete, where the direct index just dereferences. When the key space is day-unique
and near-dense, indexing beats even a good hash. The inline variant stores whole order
records in the pages and skips the second indirection, but at ~10 GB of sparse pages the
TLB pressure eats the win; the pooled variant keeps the live set compact and is both
faster and 7× smaller. The `itch-replay --book` tool (mmap file, BBO tracking on) does
the same day at 12.8 M msg/s.

### C++, apply latency

Per-operation apply latency (rdtsc via
[tsc-latency](https://github.com/groovg/tsc-latency), uncorrected, includes the ~10 ns
timestamp-pair floor; ns):

| op | count | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|---|---|
| add | 118.6M | 100 | 170 | 380 | 537 | 3,728 | 51.6 ms |
| reduce (E/C/X) | 8.6M | 40 | 110 | 309 | 514 | 954 | 152 µs |
| delete | 114.4M | 60 | 140 | 358 | 604 | 4,175 | 2.4 ms |
| replace | 21.6M | 140 | 287 | 567 | 865 | 4,235 | 1.5 ms |

The reduce p50 of 40 ns is the O(1) level-handle path. The p99.99 band is deep sorted-
vector `memmove`s and fresh page allocations; the millisecond maxima are OS scheduler
preemptions. Nothing was pinned or isolated, and a single uncorrected run over 268M
messages will catch a few.

### Published numbers elsewhere

charles-cooper/itch-order-book reports 61 ns/tick (~16.4 M msg/s) on a 2012 i7-3820 with
aggregate-only levels and a 4.4 GB preallocated ref array; CppTrader reports 3.2 M msg/s
for its reference book and ~9.8 M for its stripped benchmark variant on an i7-4790K.
Different hardware and different feature sets, so the numbers are not directly
comparable; this implementation keeps FIFO queues, bounded memory and the feed-safety
checks on at all times.

## Limitations

- Replay, not a live feed handler. `StreamParser` reassembles frames split across
  arbitrary chunk boundaries, but there is no MoldUDP64/SoupBinTCP session layer on top,
  no A/B feed arbitration, no gap or retransmission requests.
- Book-affecting messages, trades and trade voids (`P`/`Q`/`B`), trading actions and
  operational halts (`H`/`h`), NOII (`I`), Reg SHO (`Y`) and LULD collars (`J`) are
  decoded; market participant positions, MWCB, IPO quoting, RPII and direct listing
  messages (`L`/`V`/`W`/`K`/`N`/`O`) are framed and counted but not decoded.
- Order references are trusted to be locate-consistent (the order's stored locate wins
  over the message header on E/X/D/U, so a corrupt feed cannot cross-corrupt books).
- Single-threaded by design; shard symbols across instances above the library if needed.
- Latency numbers above are from an unpinned desktop Windows box with boost clocks on.

## Production notes

What would change for a live deployment: MoldUDP64 with A/B arbitration and gap-fill
feeding `StreamParser`; pinned cores, huge pages for the order pool, and an `io_uring`
read path on Linux; per-symbol sharding with an SPSC handoff per shard.

## License

MIT
