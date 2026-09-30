// Drives the real book and the reference with the same random order flow and
// checks after every step that they agree on everything visible from outside.

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <vector>

#include "lob/market.hpp"
#include "reference_market.hpp"

using namespace lob;

namespace {

constexpr std::uint16_t kBooks = 3;
constexpr Price kMid = 10'000;

class Differential : public ::testing::TestWithParam<std::uint64_t> {
protected:
    Differential() : fast_(kBooks), slow_(kBooks), rng_(GetParam()) {}

    std::uint32_t uniform(std::uint32_t lo, std::uint32_t hi) {
        return std::uniform_int_distribution<std::uint32_t>(lo, hi)(rng_);
    }

    // Prices cluster in a narrow band so levels fill up, empty out and get
    // recreated all the time.
    Price random_price() { return kMid - 20 + uniform(0, 40); }

    OrderId pick_live() { return live_[uniform(0, static_cast<std::uint32_t>(live_.size() - 1))]; }

    void forget(OrderId id) {
        auto it = std::find(live_.begin(), live_.end(), id);
        *it = live_.back();
        live_.pop_back();
    }

    void step() {
        const std::uint32_t roll = uniform(0, 99);
        if (live_.empty() || roll < 45) {
            const OrderId id = next_id_++;
            const auto book = static_cast<std::uint16_t>(uniform(0, kBooks - 1));
            const Side side = uniform(0, 1) ? Side::Buy : Side::Sell;
            const Price price = random_price();
            const Qty qty = uniform(1, 500);
            ASSERT_EQ(fast_.add(book, id, side, price, qty), slow_.add(book, id, side, price, qty));
            live_.push_back(id);
        } else if (roll < 65) {
            const OrderId id = pick_live();
            // sometimes more than is resting, which removes the order
            const Qty qty = uniform(1, 600);
            ASSERT_EQ(fast_.reduce(id, qty), slow_.reduce(id, qty));
            if (!slow_.quantity(id)) forget(id);
        } else if (roll < 85) {
            const OrderId id = pick_live();
            ASSERT_EQ(fast_.remove(id), slow_.remove(id));
            forget(id);
        } else if (roll < 97) {
            const OrderId old_id = pick_live();
            const OrderId new_id = next_id_++;
            const Price price = random_price();
            const Qty qty = uniform(1, 500);
            ASSERT_EQ(fast_.replace(old_id, new_id, price, qty), slow_.replace(old_id, new_id, price, qty));
            forget(old_id);
            live_.push_back(new_id);
        } else {
            // ids that were never issued or are already gone
            const OrderId id = next_id_ + uniform(1, 1000);
            ASSERT_EQ(fast_.reduce(id, 10), slow_.reduce(id, 10));
            ASSERT_EQ(fast_.remove(id), slow_.remove(id));
            ASSERT_EQ(fast_.replace(id, id + 1, kMid, 10), slow_.replace(id, id + 1, kMid, 10));
        }
    }

    void compare() {
        ASSERT_EQ(fast_.live_orders(), slow_.live_orders());
        for (std::uint16_t book = 0; book < kBooks; ++book) {
            for (Side side : {Side::Buy, Side::Sell}) {
                ASSERT_EQ(fast_.depth(book, side), slow_.depth(book, side));
                const auto levels = slow_.top(book, side, 1000);
                ASSERT_EQ(fast_.top(book, side, 1000), levels);
                for (const Quote& q : levels) {
                    ASSERT_EQ(fast_.queue(book, side, q.price), slow_.queue(book, side, q.price));
                }
            }
        }
        for (OrderId id : live_) {
            ASSERT_EQ(fast_.quantity(id), slow_.quantity(id));
            ASSERT_EQ(fast_.is_next_to_trade(id), slow_.is_next_to_trade(id));
        }
    }

    Market fast_;
    reference::Market slow_;
    std::mt19937_64 rng_;
    std::vector<OrderId> live_;
    OrderId next_id_ = 1;
};

TEST_P(Differential, BooksAgreeAfterEveryOperation) {
    for (int i = 0; i < 8'000; ++i) {
        ASSERT_NO_FATAL_FAILURE(step());
        ASSERT_NO_FATAL_FAILURE(compare()) << "after operation " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(Seeds, Differential, ::testing::Values(1, 2, 3, 4, 5, 6));

}  // namespace
