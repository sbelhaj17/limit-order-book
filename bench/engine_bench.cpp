// Matching engine on synthetic order flow. The replay tool measures the book
// on real data; this measures the engine, which the feed never exercises.

#include <benchmark/benchmark.h>

#include <random>
#include <vector>

#include "lob/engine.hpp"

using namespace lob;

namespace {

// Counts fills so the compiler cannot throw the matching away.
struct Tally : Listener {
    std::uint64_t fills = 0;
    void on_fill(const Fill&) { ++fills; }
};

constexpr std::uint16_t kBook = 0;
constexpr Price kMid = 100'000;

struct Op {
    enum Kind : std::uint8_t { Limit, Ioc, Market, Cancel, Reduce } kind;
    Side side;
    OrderId id;
    Price price;
    Qty qty;
};

// Roughly the mix a busy stock sees: mostly passive orders near the touch and
// cancels, with a steady trickle of orders that take liquidity.
std::vector<Op> make_flow(std::size_t n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    auto pick = [&](std::uint32_t lo, std::uint32_t hi) {
        return std::uniform_int_distribution<std::uint32_t>(lo, hi)(rng);
    };
    std::vector<Op> ops;
    ops.reserve(n);
    OrderId next = 1;
    for (std::size_t i = 0; i < n; ++i) {
        const Side side = pick(0, 1) ? Side::Buy : Side::Sell;
        const std::uint32_t roll = pick(0, 99);
        if (roll < 48) {
            // passive: somewhere in the ten ticks behind the touch
            const Price offset = pick(1, 10);
            const Price price = side == Side::Buy ? kMid - offset : kMid + offset;
            ops.push_back({Op::Limit, side, next++, price, pick(1, 10) * 100});
        } else if (roll < 86) {
            // cancel something recent; it may already have traded
            const OrderId back = pick(1, 2000);
            ops.push_back({Op::Cancel, side, next > back ? next - back : 1, 0, 0});
        } else if (roll < 91) {
            ops.push_back({Op::Reduce, side, next > 50 ? next - pick(1, 50) : 1, 0, 100});
        } else if (roll < 98) {
            // crosses up to three ticks into the other side
            const Price through = pick(1, 3);
            const Price price = side == Side::Buy ? kMid + through : kMid - through;
            ops.push_back({Op::Ioc, side, next++, price, pick(1, 5) * 100});
        } else {
            ops.push_back({Op::Market, side, next++, 0, pick(1, 3) * 100});
        }
    }
    return ops;
}

void seed_book(Engine<Tally>& engine, OrderId first_id) {
    OrderId id = first_id;
    for (Price offset = 1; offset <= 20; ++offset) {
        for (int k = 0; k < 20; ++k) {
            engine.limit(kBook, id++, Side::Buy, kMid - offset, 500);
            engine.limit(kBook, id++, Side::Sell, kMid + offset, 500);
        }
    }
}

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
