// Runs BM_MixedFlow's order flow through the engine once, untimed, and counts
// what each kind of operation actually did. The benchmark reports one time per
// operation; this is what those operations were.
//
//   ./build/flow_mix > bench/flow_mix.txt

#include <cstdio>
#include <string>

#include "flow.hpp"
#include "lob/engine.hpp"

using namespace lob;
using namespace lob::bench;

namespace {

struct Outcomes : Listener {
    std::uint64_t rests = 0, fills = 0, rejects = 0;
    void on_rest(OrderId, Qty) { ++rests; }
    void on_fill(const Fill&) { ++fills; }
    void on_reject(OrderId, Reject) { ++rejects; }
};

struct Row {
    const char* name;
    std::uint64_t ops = 0, rested = 0, traded = 0, fills = 0, rejected = 0;
};

std::string commas(std::uint64_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
    return s;
}

double percent(std::uint64_t part, std::uint64_t whole) {
    return whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
}

}  // namespace

int main() {
    // same size, seed and starting book as BM_MixedFlow
    constexpr std::size_t kOps = 1'000'000;
    const auto ops = make_flow(kOps, 42);
    Outcomes out;
    Engine<Outcomes> engine(1, out);
    seed_book(engine, 1'000'000'000);
    const std::size_t seeded = engine.book().live_orders();

    Row rows[] = {{"passive limit"}, {"ioc"}, {"market"}, {"cancel"}, {"reduce"}};
    for (const Op& op : ops) {
        const Outcomes before = out;
        switch (op.kind) {
        case Op::Limit: engine.limit(kBook, op.id, op.side, op.price, op.qty); break;
        case Op::Ioc: engine.limit(kBook, op.id, op.side, op.price, op.qty, TimeInForce::IOC); break;
        case Op::Market: engine.market(kBook, op.id, op.side, op.qty); break;
        case Op::Cancel: engine.cancel(op.id); break;
        case Op::Reduce: engine.reduce(op.id, op.qty); break;
        }
        Row& row = rows[op.kind];
        ++row.ops;
        row.rested += out.rests - before.rests;
        row.fills += out.fills - before.fills;
        row.traded += out.fills > before.fills ? 1 : 0;
        row.rejected += out.rejects - before.rejects;
    }

    std::printf("BM_MixedFlow's flow: %s operations, seed 42, on a book seeded with %s orders\n\n",
                commas(kOps).c_str(), commas(seeded).c_str());
    std::printf("%-14s %9s %7s %9s %9s %9s %18s\n", "operation", "count", "share", "rested", "traded", "fills",
                "rejected");
    for (const Row& r : rows) {
        char rejected[32];
        std::snprintf(rejected, sizeof rejected, "%s (%.1f%%)", commas(r.rejected).c_str(),
                      percent(r.rejected, r.ops));
        std::printf("%-14s %9s %6.1f%% %9s %9s %9s %18s\n", r.name, commas(r.ops).c_str(), percent(r.ops, kOps),
                    commas(r.rested).c_str(), commas(r.traded).c_str(), commas(r.fills).c_str(), rejected);
    }
    std::printf("\n'traded' counts operations that got at least one fill. A cancel or reduce is\n"
                "rejected when its id is not resting: it already traded or was cancelled, or it\n"
                "was an IOC or market order, which never rests.\n\n");
    std::printf("resting at the end: %s orders, %zu bid levels, %zu ask levels\n",
                commas(engine.book().live_orders()).c_str(), engine.book().top(kBook, Side::Buy, 1000).size(),
                engine.book().top(kBook, Side::Sell, 1000).size());
    return 0;
}
