# limit-order-book

A limit order book and matching engine in C++20, checked against a full day of NASDAQ market data.

There are two halves. `Market` keeps the books: orders, price levels, queues. `Engine` sits on top and does the matching: limit orders (day or IOC), market orders, cancels, in-place size reductions and replaces, all in price-time priority. There is also a parser for NASDAQ's TotalView-ITCH 5.0 feed and a tool, `itch_replay`, that rebuilds every book on the exchange from a day of it.

## Results on a real day

NASDAQ publishes a few sample days of ITCH. I used 30 December 2019 (`scripts/get_itch.sh` fetches it).

| | |
|---|---|
| messages | 268,744,780 |
| instruments | 8,906 |
| peak resting orders | 1,924,078 |
| time, on an Apple M4 | 17.6 s of user CPU, about 66 ns per message (15 M messages/s) |
| parsing alone | 1.4 s of user CPU, about 5 ns per message |
| messages that referred to an order the book did not have | 0 |
| orders left in the book after the close | 0 |
| order-executed (`E`) messages that hit the order at the front of the best level | 5,722,824 of 5,722,824 |

The last row is the check I trust most. An exchange matches in price-time priority, so an execution should hit whichever order my book has first in line at the best price. If any queue were built wrong, whether from a parsing bug, a wrong offset, or a replace handled badly, some executions would land on an order the book thinks is second or third.

It covers order-executed (`E`) messages only, not every execution in the feed. An order executed at a price other than the one it was displayed at comes as a `C` message, which the replay applies to the book but does not check, and trades against hidden orders (`P`) never touch the book at all.

Times are user CPU time (`ru_utime` from `getrusage`), which leaves out the kernel's time paging the file in. The wall clock is less useful here: the unpacked file is 8.3 GB, and with a browser open my laptop could not keep it all in the page cache, so a replay spent as long waiting on the disk as it did running (17 s wall with the file cached, 33 s once it no longer fit).

The parse-only figure is about a tenth of the 51 ns per message in the commit that added the tool (`096b370`). That one was wall time on the first 1.5 GB of the file, so it included waiting for pages of the mapped file, and a parse that does almost nothing per message is the run where that matters most. The parser has only gained work since then (the lookahead cursor), so I put the gap down to how it was measured rather than to the code. I have not re-timed it to check.

## The 0.2% that didn't match

The first version queued each order behind whatever was already at its price, in the order the feed showed them. That gave 5,712,259 of 5,722,824 executions at the front of the queue, 99.815%. The other 10,565 were always at the best price, so the right level but the wrong order within it. About four in five came in the first twenty minutes after the open.

In every one of the 10,565, the order that traded had the lowest order reference number at its price. The spec says the reference number is assigned when NASDAQ receives the order. So those orders reached the exchange before the ones ahead of them in my queue, but showed up in the feed later. My reading is that they were entered before the open, or during a halt, and only published once trading started, behind orders that came in later but were already on the book.

`Market::add` now takes a `Rank`. With `Rank::Id` an order goes ahead of any order at its price with a larger id; the replay uses that, and it brings the count to 5,722,824 of 5,722,824. With `Rank::Arrival`, the default, an order joins the back of the queue, which is what the matching engine wants.

That 100% is weaker evidence than it looks. I found the rule by looking at this day's misses and checked it on the same day, and since every miss was the lowest id at its price, queueing by id fixes those 10,565 by construction. What the replay does show is that the change broke none of the other 5,712,259. I have not tried it on a second day.

## How it got faster

Each row is a commit. The first number is the one in that commit's message: wall time on the first 1.5 GB of the file (48M messages), not the full day. I have not yet gone back and re-timed each commit on the full day with user CPU time, so that column only has the last row, which is the code the results above come from.

| commit | change | ns per message, 1.5 GB slice, wall | full day, user CPU |
|---|---|---|---|
| `096b370` | `std::map` per side, `std::list` per level, `std::unordered_map` for ids | 285 | not redone yet |
| `cde3aa3` | orders in a pool, intrusive doubly linked queue per level | 257 | not redone yet |
| `1f69eb8` | open-addressing hash table for ids | ~130 | not redone yet |
| `09ee4e0` | price levels in a sorted vector with the best price at the back | 77 | not redone yet |
| `b744639` | prefetch the id table a few messages ahead | ~60 | not redone yet |
| `6624666` | queue by id (for correctness, not speed) | not timed | 66 |

On the slice that is 285 down to ~60, about 4.7x. The two columns are measured differently, so a ratio across them would mean nothing.

What mattered, in order:

- **The id table.** Almost every message looks up an order by id, and `std::unordered_map` chases a pointer per lookup. Linear probing over one flat array halved the time. Deletion shifts the rest of the run back instead of leaving tombstones, because nearly every order is deleted again within the day.
- **The price levels.** Each side of a book is a `std::vector` of (key, level) sorted so the best price is at the back. New orders mostly arrive at or near the best price, so finding the level is a short walk from the back and inserting a new level barely moves anything. Ask prices are stored bit-flipped so that a better price is a larger key on both sides, and the code never branches on the side.
- **Prefetching.** The id table is 64 MB, far bigger than cache, so every lookup was a cache miss the CPU sat waiting for. `itch::parse` can run a second cursor a few messages ahead and hand each upcoming id to the handler, which prefetches that slot.

Things I tried that did not help:

- Hashing ids by masking off the low bits instead of multiplying, hoping to keep nearby ids close in memory: 2.5x slower on a 1.5 GB slice of the file. ITCH ids are nearly sequential, so they formed one huge run, and every deletion walked it.
- Hashing in blocks of four so neighbouring ids share a cache line: no change.
- Prefetching the order record as well as its id slot: slightly slower, because it needed an extra lookup.
- A binary search for orders far from the best price: no measurable difference. Most adds land within a few levels of the top of the book.

## The engine

`bench/engine_bench.cpp` runs the engine on synthetic flow for a single book, since a market data feed never exercises matching. The times are per operation, as Google Benchmark reports them on the M4:

| benchmark | per operation |
|---|---|
| mixed flow, 1M operations: 48% passive limits, 38% cancels, 5% reduces, 7% IOC, 2% market | 19.5 ns |
| add an order behind the best price, then cancel it | 16 ns (31 ns per pair) |
| an IOC that clears three levels, then the three levels refilled | 35 ns (140 ns per round) |

Not much of the mixed flow is matching. Every passive limit sits one to ten ticks behind a fixed mid on its own side, so none of them ever crosses; only the 9% that are IOC or market orders trade. The cancels and reduces name one of the last 2,000 or 50 ids handed out, and many of those have already gone or were IOC or market orders that never rested, so 44% of the cancels and 17% of the reduces are rejected after one failed lookup in the id table. `bench/flow_mix.cpp` runs the same flow once without timing it and counts all of this (`bench/flow_mix.txt`). So the 19.5 ns is an average over operations that mostly rest an order, cancel or shrink one, or get rejected; fewer than one in ten matches.

## Testing

- `tests/reference_market.hpp` is the first version of the book, `std::map` and `std::list`, slow but short enough to be obviously right. The `Market` unit tests are typed tests that run against both it and the real book. The engine, id table and ITCH unit tests do not use it.
- `tests/reference_engine.hpp` is a second matching engine written as plainly as I could. The differential tests feed both engines, and both books, the same random order flow and require identical output after every operation: same fills, same book, same queues.
- The id table is checked against `std::unordered_map` under churn, including a tiny table where runs wrap around the end constantly.
- CI builds on Linux and macOS and runs the unit tests normally and under AddressSanitizer and UBSan. The sanitizer build leaves out the benchmarks; the normal build runs them once as a smoke test. No CI job runs `itch_replay` or touches real ITCH data (the parser tests build their messages byte by byte), so the replay results above come from my machine only.

## Building

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
./build/engine_bench
```

```
scripts/get_itch.sh
./build/itch_replay data/12302019.NASDAQ_ITCH50
./build/itch_replay data/12302019.NASDAQ_ITCH50 --symbol AAPL --at 10:30:00
```

CMake downloads GoogleTest and Google Benchmark on the first configure.

## Not done

- One thread, no network. A real venue has a gateway, a sequencer and a market data publisher around this.
- No self-trade prevention, no fill-or-kill or post-only orders.
- Ids of orders that have left the book can be reused. A real exchange would reject that.
- Prices are 32-bit ticks, which is what ITCH uses, so nothing above $429,496.
