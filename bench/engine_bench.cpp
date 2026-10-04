// Matching engine on synthetic order flow. The replay tool measures the book
// on real data; this measures the engine, which the feed never exercises.

#include <benchmark/benchmark.h>

#include "flow.hpp"
#include "lob/engine.hpp"

using namespace lob;
using namespace lob::bench;

namespace {

// Counts fills so the compiler cannot throw the matching away.
struct Tally : Listener {
    std::uint64_t fills = 0;
    void on_fill(const Fill&) { ++fills; }
};

void BM_MixedFlow(benchmark::State& state) {
    const auto ops = make_flow(static_cast<std::size_t>(state.range(0)), 42);
    std::uint64_t fills = 0;
    for (auto _ : state) {
        state.PauseTiming();
        Tally tally;
        Engine<Tally> engine(1, tally);
        seed_book(engine, 1'000'000'000);
        state.ResumeTiming();

        for (const Op& op : ops) {
            switch (op.kind) {
            case Op::Limit: engine.limit(kBook, op.id, op.side, op.price, op.qty); break;
            case Op::Ioc: engine.limit(kBook, op.id, op.side, op.price, op.qty, TimeInForce::IOC); break;
            case Op::Market: engine.market(kBook, op.id, op.side, op.qty); break;
            case Op::Cancel: engine.cancel(op.id); break;
            case Op::Reduce: engine.reduce(op.id, op.qty); break;
            }
        }
        fills += tally.fills;
        benchmark::DoNotOptimize(engine.book().live_orders());
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * state.range(0));
    state.counters["fills/op"] =
        benchmark::Counter(static_cast<double>(fills) / static_cast<double>(state.iterations() * state.range(0)));
}
BENCHMARK(BM_MixedFlow)->Arg(1'000'000)->Unit(benchmark::kMillisecond);

// The cheapest round trip: rest an order behind the touch, then cancel it.
void BM_AddThenCancel(benchmark::State& state) {
    Tally tally;
    Engine<Tally> engine(1, tally);
    seed_book(engine, 1'000'000'000);
    OrderId id = 1;
    for (auto _ : state) {
        engine.limit(kBook, id, Side::Buy, kMid - 5, 100);
        engine.cancel(id);
        ++id;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * 2);
}
BENCHMARK(BM_AddThenCancel);

// A marketable order that clears three price levels, then the three levels
// being put back. Pausing the timer around the refill costs more than the
// refill itself, so both are timed and counted: four orders per iteration.
void BM_SweepThreeLevelsAndRefill(benchmark::State& state) {
    Tally tally;
    Engine<Tally> engine(1, tally);
    OrderId id = 1;
    for (Price p = 1; p <= 3; ++p) engine.limit(kBook, id++, Side::Sell, kMid + p, 100);
    for (auto _ : state) {
        engine.limit(kBook, id++, Side::Buy, kMid + 3, 300, TimeInForce::IOC);
        for (Price p = 1; p <= 3; ++p) engine.limit(kBook, id++, Side::Sell, kMid + p, 100);
    }
    benchmark::DoNotOptimize(tally.fills);
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * 4);
}
BENCHMARK(BM_SweepThreeLevelsAndRefill);

}  // namespace

BENCHMARK_MAIN();
