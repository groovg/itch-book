# itch-book

[![CI](https://github.com/groovg/itch-book/actions/workflows/ci.yml/badge.svg)](https://github.com/groovg/itch-book/actions/workflows/ci.yml)

NASDAQ TotalView-ITCH 5.0 feed handler and limit order book reconstruction in C++23.
Header-only, no dependencies beyond [fixed-decimal](https://github.com/groovg/fixed-decimal)
for exact prices. Parses the raw `BinaryFILE` day dumps NASDAQ publishes at
[emi.nasdaq.com/ITCH](https://emi.nasdaq.com/ITCH/) and maintains per-symbol books with
FIFO order queues, aggregate price levels and best bid/offer tracking.

On a full trading day (`12302019.NASDAQ_ITCH50`, 268.7M messages, 8.25 GB) it replays
parse + full book apply for all 8,907 symbols at **~16.8M messages/s single-threaded**
(~60 ns/message) inside **~1.4 GB** of book structures, with zero unresolved order
references and zero crossed books at the close.

## Usage

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

Handlers are plain structs; implement only the callbacks you need. Messages you skip cost
one length lookup — nothing is decoded for them:

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

The manager can also emit a time-and-sales stream — `E` executions print at the *resting
order's* price, which only the book knows, plus printable `C`, non-cross trades, crosses
and broken-trade voids, all in feed order:

```cpp
itch::BookManager tape(nullptr, [](const itch::TradePrint& t) { /* ... */ });
```

Build and test:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Tools: `itch-replay <file> [--book]` (per-type counts, or full book replay with stats),
`gen-synthetic <out> <messages> [symbols] [seed]` (deterministic test feed),
`parse_throughput` / `book_throughput` / `apply_latency` benches (the last needs
`-DITCH_BENCH_LATENCY=ON`, x86 only). GCC and Clang; MSVC is out because the price
type needs `__int128`.

## Wire format notes

Every message sits behind a 2-byte big-endian length prefix; a zero length marks end of
session. The parser treats the prefix as authoritative: known types are additionally
checked against their fixed spec length (one table lookup), unknown or mismatched frames
are skipped by length and counted, never parsed. That matters in practice — Nasdaq adds
message types over the years (`O`, Direct Listing with Capital Raise, arrived in 2023),
and parsers that abort on unknown bytes die on the first file recorded after their
spec revision.

All multi-byte fields are big-endian at odd offsets. Fields are decoded with
`memcpy` into an integer plus `std::byteswap` — with `-O2` GCC and Clang compile that to
the same single `mov + bswap` a `reinterpret_cast` of a packed struct would produce, but
without the unaligned-access UB, so the whole hot path runs clean under UBSan and works
on strict-alignment targets. The 6-byte timestamps are copied as 6 bytes; the popular
trick of one 8-byte load at offset 5 shifted right reads past the end of the buffer on
the final 12-byte message of a file.

Dispatch is a `switch` on the type byte into a compile-time handler concept
(`if constexpr (requires { h.on_add(...); })`), so decode inlines straight into book
application. There are no virtual calls anywhere on the hot path.

Prices are ITCH `Price(4)` — 32-bit unsigned with four implied decimals — and land in
`fixed_decimal::Fixed<4, PriceTag, int64_t>`: exact integer mantissa arithmetic, no
floats, `from_raw` costs nothing.

## Book design

- **Books** live in a flat vector indexed by `stock locate` — the spec defines it as a
  dense, day-scoped array index, so no symbol hashing ever happens per message.
- **Price levels** per side are a sorted vector with the best level at the back. Ask
  prices are stored negated so both sides share one ascending comparison and the same
  scan-from-back loop. Adds and deletes overwhelmingly hit within a few levels of the
  touch, so the linear scan typically ends in 1–5 comparisons; a deep insert pays an
  O(levels) `memmove`, which the latency table below quantifies honestly.
- **Level records** (aggregate shares, order count, FIFO head/tail) are pooled per book
  behind 32-bit handles with a LIFO freelist — no allocation per level after warm-up,
  and handles stay valid across vector growth.
- **Orders carry their level handle**, so executes, cancels, deletes and replaces never
  search the book: one order lookup, one level dereference. Messages that mutate an
  existing order are more than half of a NASDAQ day (deletes alone are ~43%), which is
  why this is the property worth paying for.
- **FIFO queues** are intrusive doubly-linked chains of order references per level, so
  queue position is reconstructible — that is the part aggregate-only books throw away.
- **The order-reference index** exploits that ITCH refs are day-unique and near-dense:
  a paged direct index (8,192 refs per page) instead of a hash. The default store keeps
  pages of 32-bit handles into a recycled order pool; a page is freed to a spare list the
  moment its last order dies, so resident memory is bounded by the *live window* of the
  ref space, not by the day's 118M adds. A cap on the accepted ref space (`kMaxRef`)
  keeps a corrupt or adversarial feed from growing the page table without bound.
- `reserve()` on the manager and the store pre-sizes everything for a strict
  zero-allocation steady state (verified by a test that counts global `operator new`
  calls across 400k messages after warm-up: zero).
- **Trading-action state** (`H`) is tracked per locate and queryable
  (`trading_state(locate)`), but books are deliberately not gated on it: Nasdaq keeps
  order maintenance flowing during halts, so a handler that stops applying messages on
  `H` resumes with a corrupt book.

Robustness rules: unknown refs are counted and ignored, duplicate adds replace the stale
order, over-sized executes clamp, zero-share or zero-price messages are rejected — each
path is unit-tested and mirrored exactly by the reference implementation used for
differential testing.

## Correctness

- **Differential test**: a deliberately naive reference book (`std::map` levels,
  `std::unordered_map` orders, ~100 lines) consumes the same synthetic feeds as the fast
  book; full book states (every level, every side, live-order counts) are compared at
  checkpoints. Runs over 3 seeds × 400k messages × all three order-store variants.
- **Structural invariants** (`Book::validate`): sorted sides, level aggregates equal to
  the sum of their FIFO chain, link consistency, order counts.
- **Real-data smoke**: the full NASDAQ and BX days replay with zero missing refs, zero
  duplicates, zero rejects, zero clamps, and zero crossed books at the close.
- **Fuzzing**: a libFuzzer harness drives `parse` + book apply in CI (ASan+UBSan);
  a deterministic mutation test (bit flips + truncations over a synthetic feed) runs in
  the regular suite. The framing layer never reads outside the buffer by construction —
  decode only happens after the length check.
- CI: GCC, Clang, ASan+UBSan, fuzz — all on every push.

## Benchmarks

Machine: AMD Ryzen 9 9950X3D (Zen 5), Windows 11, GCC 16.1 `-O3`, single thread, no core
isolation. Input: `12302019.NASDAQ_ITCH50` (268,744,780 messages, 8.25 GB) fully resident
in a RAM buffer, so no IO or page-cache effects in the measured loop. Reproduce with
`parse_throughput <file>` and `book_throughput <file> <variant>`.

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

Where the factors come from: replacing `std::map` levels with the sorted vector is
~1.6× (touch-local scans instead of pointer chasing); replacing the hash ref-index with
paged direct indexing is another ~3× (one arithmetic dereference, no hashing, no probe
chains, no rehash stalls — and near-monotonic refs keep the hot pages cached). The flat
hash (fibonacci hashing, linear probing, backward-shift deletion) isolates how much of
the `unordered_map` cost is the container itself: dropping per-node allocation and
bucket-chain chasing buys ~1.7×, but it still hashes, probes and moves 40-byte slots on
every delete, where the direct index just dereferences — when the key space is day-unique
and near-dense, indexing beats even a good hash. The inline variant stores whole order
records in the pages and skips the second indirection, but at ~10 GB of sparse pages the
TLB pressure eats the win; the pooled variant keeps the live set compact and is both
faster and 7× smaller. The `itch-replay --book` tool — mmap file, BBO tracking on — does
the same day at 12.8 M msg/s.

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
preemptions — nothing was pinned or isolated, and a single uncorrected run over 268M
messages will catch a few.

For context, published single-threaded parse+apply numbers elsewhere:
charles-cooper/itch-order-book reports 61 ns/tick (~16.4 M msg/s) on a 2012 i7-3820 with
aggregate-only levels and a 4.4 GB preallocated ref array; CppTrader reports 3.2 M msg/s
for its reference book and ~9.8 M for its stripped benchmark variant on an i7-4790K.
Different hardware, different feature sets — the numbers are not directly comparable,
and this implementation keeps FIFO queues, bounded memory and feed-robustness checks on
at all times.

## Limitations

- **Replay, not a live feed handler.** `StreamParser` reassembles frames split across
  arbitrary chunk boundaries, but there is no MoldUDP64/SoupBinTCP session layer on top —
  no A/B feed arbitration, no gap or retransmission requests.
- Book-affecting messages, trades and trade voids (`P`/`Q`/`B`) and trading actions (`H`)
  are decoded; NOII, RegSHO, LULD and the other administrative types are framed and
  counted but not decoded.
- Order references are trusted to be locate-consistent (the order's stored locate wins
  over the message header on E/X/D/U, so a corrupt feed cannot cross-corrupt books).
- Single-threaded by design; shard symbols across instances above the library if needed.
- Latency numbers above are from an unpinned desktop Windows box with boost clocks on.

## What I would do differently in production

MoldUDP64 with A/B arbitration and gap-fill feeding `StreamParser`; pinned cores, huge
pages for the order pool, and an `io_uring` read path on Linux; per-symbol sharding with
an SPSC handoff per shard.

## License

MIT
