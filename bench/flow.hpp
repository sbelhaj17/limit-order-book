#pragma once

// The synthetic order flow behind BM_MixedFlow. It lives here so that
// flow_mix can run exactly the same flow and count what it does.

#include <cstdint>
#include <random>
#include <vector>

#include "lob/engine.hpp"

namespace lob::bench {

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
inline std::vector<Op> make_flow(std::size_t n, std::uint64_t seed) {
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

template <class L>
void seed_book(Engine<L>& engine, OrderId first_id) {
    OrderId id = first_id;
    for (Price offset = 1; offset <= 20; ++offset) {
        for (int k = 0; k < 20; ++k) {
            engine.limit(kBook, id++, Side::Buy, kMid - offset, 500);
            engine.limit(kBook, id++, Side::Sell, kMid + offset, 500);
        }
    }
}

}  // namespace lob::bench
