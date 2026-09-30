#include "lob/engine.hpp"

#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "reference_engine.hpp"

using namespace lob;
using reference::Event;
using reference::Recorder;

namespace {

constexpr std::uint16_t kBook = 0;

class EngineTest : public ::testing::Test {
protected:
    // returns the events produced since the last call
    std::vector<Event> take() {
        std::vector<Event> out;
        out.swap(rec.events);
        return out;
    }

    static Event rest(OrderId id, Qty qty) { return {Event::Rest, id, 0, 0, qty}; }
    static Event fill(OrderId resting, OrderId incoming, Price price, Qty qty) {
        return {Event::Filled, resting, incoming, price, qty};
    }
    static Event cancel(OrderId id, Qty qty) { return {Event::Cancel, id, 0, 0, qty}; }
    static Event reject(OrderId id, Reject why) { return {Event::Rejected, id, 0, 0, static_cast<Qty>(why)}; }

    using Events = std::vector<Event>;

    Recorder rec;
    Engine<Recorder> engine{2, rec};
};

TEST_F(EngineTest, OrdersThatDoNotCrossRest) {
    engine.limit(kBook, 1, Side::Buy, 9990, 100);
    engine.limit(kBook, 2, Side::Sell, 10010, 100);
    EXPECT_EQ(take(), (Events{rest(1, 100), rest(2, 100)}));
    EXPECT_EQ(engine.book().best(kBook, Side::Buy)->price, 9990u);
    EXPECT_EQ(engine.book().best(kBook, Side::Sell)->price, 10010u);
}

TEST_F(EngineTest, TradesAtTheRestingPrice) {
    engine.limit(kBook, 1, Side::Sell, 10000, 100);
    take();
    // willing to pay 10050, but the offer was 10000
    engine.limit(kBook, 2, Side::Buy, 10050, 100);
    EXPECT_EQ(take(), (Events{fill(1, 2, 10000, 100)}));
    EXPECT_FALSE(engine.book().best(kBook, Side::Sell));
    EXPECT_FALSE(engine.book().best(kBook, Side::Buy));
}

TEST_F(EngineTest, BetterPricesFillFirst) {
    engine.limit(kBook, 1, Side::Sell, 10020, 100);
    engine.limit(kBook, 2, Side::Sell, 10000, 100);
    engine.limit(kBook, 3, Side::Sell, 10010, 100);
    take();
    engine.limit(kBook, 4, Side::Buy, 10020, 250);
    EXPECT_EQ(take(), (Events{fill(2, 4, 10000, 100), fill(3, 4, 10010, 100), fill(1, 4, 10020, 50)}));
    EXPECT_EQ(engine.book().quantity(1), 50u);
}

TEST_F(EngineTest, EarlierOrdersFillFirstAtTheSamePrice) {
    engine.limit(kBook, 1, Side::Buy, 10000, 100);
    engine.limit(kBook, 2, Side::Buy, 10000, 100);
    engine.limit(kBook, 3, Side::Buy, 10000, 100);
    take();
    engine.limit(kBook, 4, Side::Sell, 10000, 150);
    EXPECT_EQ(take(), (Events{fill(1, 4, 10000, 100), fill(2, 4, 10000, 50)}));
    // order 2 was partly filled and is still first in line
    EXPECT_TRUE(engine.book().is_next_to_trade(2));
    EXPECT_EQ(engine.book().quantity(2), 50u);
}

TEST_F(EngineTest, RemainderRestsAtItsLimit) {
    engine.limit(kBook, 1, Side::Sell, 10000, 100);
    take();
    engine.limit(kBook, 2, Side::Buy, 10010, 300);
    EXPECT_EQ(take(), (Events{fill(1, 2, 10000, 100), rest(2, 200)}));
    auto bid = engine.book().best(kBook, Side::Buy);
    ASSERT_TRUE(bid);
    EXPECT_EQ(bid->price, 10010u);
    EXPECT_EQ(bid->qty, 200u);
}

TEST_F(EngineTest, DoesNotTradeThroughTheLimit) {
    engine.limit(kBook, 1, Side::Sell, 10000, 100);
    engine.limit(kBook, 2, Side::Sell, 10030, 100);
    take();
    engine.limit(kBook, 3, Side::Buy, 10020, 500);
    EXPECT_EQ(take(), (Events{fill(1, 3, 10000, 100), rest(3, 400)}));
    EXPECT_EQ(engine.book().best(kBook, Side::Sell)->price, 10030u);
}

TEST_F(EngineTest, ImmediateOrCancelNeverRests) {
    engine.limit(kBook, 1, Side::Sell, 10000, 100);
    take();
    engine.limit(kBook, 2, Side::Buy, 10000, 300, TimeInForce::IOC);
    EXPECT_EQ(take(), (Events{fill(1, 2, 10000, 100), cancel(2, 200)}));
    EXPECT_FALSE(engine.book().best(kBook, Side::Buy));

    engine.limit(kBook, 3, Side::Buy, 10000, 50, TimeInForce::IOC);
    EXPECT_EQ(take(), (Events{cancel(3, 50)}));
}

TEST_F(EngineTest, MarketOrderSweepsLevelsAndCancelsTheRest) {
    engine.limit(kBook, 1, Side::Buy, 10000, 100);
    engine.limit(kBook, 2, Side::Buy, 9900, 100);
    take();
    engine.market(kBook, 3, Side::Sell, 500);
    EXPECT_EQ(take(), (Events{fill(1, 3, 10000, 100), fill(2, 3, 9900, 100), cancel(3, 300)}));
    EXPECT_FALSE(engine.book().best(kBook, Side::Buy));
    EXPECT_FALSE(engine.book().best(kBook, Side::Sell));
}

TEST_F(EngineTest, MarketOrderIntoAnEmptyBookIsCancelled) {
    engine.market(kBook, 1, Side::Buy, 100);
    EXPECT_EQ(take(), (Events{cancel(1, 100)}));
}

TEST_F(EngineTest, Cancel) {
    engine.limit(kBook, 1, Side::Buy, 10000, 100);
    take();
    engine.cancel(1);
    EXPECT_EQ(take(), (Events{cancel(1, 100)}));
    engine.cancel(1);
    EXPECT_EQ(take(), (Events{reject(1, Reject::UnknownId)}));
}

TEST_F(EngineTest, ReduceKeepsPriority) {
    engine.limit(kBook, 1, Side::Buy, 10000, 100);
    engine.limit(kBook, 2, Side::Buy, 10000, 100);
    take();
    engine.reduce(1, 60);
    EXPECT_EQ(take(), (Events{cancel(1, 60)}));
    engine.limit(kBook, 3, Side::Sell, 10000, 40);
    EXPECT_EQ(take(), (Events{fill(1, 3, 10000, 40)}));
}

TEST_F(EngineTest, ReducingByEverythingRemovesTheOrder) {
    engine.limit(kBook, 1, Side::Buy, 10000, 100);
    take();
    engine.reduce(1, 500);
    EXPECT_EQ(take(), (Events{cancel(1, 100)}));
    EXPECT_FALSE(engine.book().quantity(1));
}

TEST_F(EngineTest, ReplaceLosesPriorityAndCanTrade) {
    engine.limit(kBook, 1, Side::Buy, 10000, 100);
    engine.limit(kBook, 2, Side::Buy, 10000, 100);
    engine.limit(kBook, 9, Side::Sell, 10020, 100);
    take();

    engine.replace(1, 3, 10000, 100);
    EXPECT_EQ(take(), (Events{cancel(1, 100), rest(3, 100)}));
    EXPECT_TRUE(engine.book().is_next_to_trade(2));

    // moving the price through the offer trades immediately
    engine.replace(2, 4, 10020, 30);
    EXPECT_EQ(take(), (Events{cancel(2, 100), fill(9, 4, 10020, 30)}));
}

TEST_F(EngineTest, ReplaceMayKeepItsId) {
    engine.limit(kBook, 1, Side::Sell, 10000, 100);
    take();
    engine.replace(1, 1, 10010, 80);
    EXPECT_EQ(take(), (Events{cancel(1, 100), rest(1, 80)}));
    EXPECT_EQ(engine.book().best(kBook, Side::Sell)->price, 10010u);
}

TEST_F(EngineTest, Rejects) {
    engine.limit(kBook, 1, Side::Buy, 10000, 100);
    take();

    engine.limit(kBook, 1, Side::Sell, 10100, 10);
    EXPECT_EQ(take(), (Events{reject(1, Reject::DuplicateId)}));
    engine.limit(kBook, 2, Side::Buy, 10000, 0);
    EXPECT_EQ(take(), (Events{reject(2, Reject::ZeroQuantity)}));
    engine.limit(7, 3, Side::Buy, 10000, 10);
    EXPECT_EQ(take(), (Events{reject(3, Reject::UnknownInstrument)}));
    engine.replace(99, 4, 10000, 10);
    EXPECT_EQ(take(), (Events{reject(4, Reject::UnknownId)}));
    engine.limit(kBook, 5, Side::Buy, 9990, 10);
    take();
    engine.replace(1, 5, 10000, 10);
    EXPECT_EQ(take(), (Events{reject(5, Reject::DuplicateId)}));

    // none of that touched the book
    EXPECT_EQ(engine.book().quantity(1), 100u);
    EXPECT_EQ(engine.book().live_orders(), 2u);
}

TEST_F(EngineTest, BooksAreSeparateMarkets) {
    engine.limit(0, 1, Side::Sell, 10000, 100);
    engine.limit(1, 2, Side::Buy, 10000, 100);
    EXPECT_EQ(take(), (Events{rest(1, 100), rest(2, 100)}));
}

// Same random flow into the engine and the reference. Their event streams
// have to match exactly, and the book must never be left crossed.
class EngineDifferential : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(EngineDifferential, SameEventsAsTheReference) {
    constexpr std::uint16_t books = 2;
    Recorder rec;
    Engine<Recorder> fast(books, rec);
    reference::Engine slow(books);

    std::mt19937_64 rng(GetParam());
    auto uniform = [&](std::uint32_t lo, std::uint32_t hi) {
        return std::uniform_int_distribution<std::uint32_t>(lo, hi)(rng);
    };
    std::vector<OrderId> ids;  // every id handed out so far, live or not
    OrderId next_id = 1;
    auto some_id = [&]() -> OrderId { return ids.empty() ? 1 : ids[uniform(0, static_cast<std::uint32_t>(ids.size() - 1))]; };

    int fills = 0;
    int rejects = 0;
    for (int i = 0; i < 40'000; ++i) {
        const auto book = static_cast<std::uint16_t>(uniform(0, books - 1));
        const Side side = uniform(0, 1) ? Side::Buy : Side::Sell;
        const Price price = 10'000 - 15 + uniform(0, 30);
        const Qty qty = uniform(0, 300);  // zero now and then, to hit the reject
        const std::uint32_t roll = uniform(0, 99);

        if (roll < 50) {
            const OrderId id = next_id++;
            const TimeInForce tif = uniform(0, 4) == 0 ? TimeInForce::IOC : TimeInForce::Day;
            fast.limit(book, id, side, price, qty, tif);
            slow.limit(book, id, side, price, qty, tif);
            ids.push_back(id);
        } else if (roll < 58) {
            const OrderId id = next_id++;
            fast.market(book, id, side, qty);
            slow.market(book, id, side, qty);
        } else if (roll < 78) {
            const OrderId id = some_id();
            fast.cancel(id);
            slow.cancel(id);
        } else if (roll < 88) {
            const OrderId id = some_id();
            fast.reduce(id, qty);
            slow.reduce(id, qty);
        } else if (roll < 98) {
            const OrderId old_id = some_id();
            const OrderId new_id = uniform(0, 9) == 0 ? old_id : next_id++;
            fast.replace(old_id, new_id, price, qty);
            slow.replace(old_id, new_id, price, qty);
            ids.push_back(new_id);
        } else {
            // reuse an id that may well still be resting
            const OrderId id = some_id();
            fast.limit(book, id, side, price, qty);
            slow.limit(book, id, side, price, qty);
        }

        ASSERT_EQ(rec.events, slow.events) << "diverged at operation " << i;
        for (const Event& e : rec.events) {
            if (e.kind == Event::Filled) ++fills;
            if (e.kind == Event::Rejected) ++rejects;
        }
        rec.events.clear();
        slow.events.clear();

        for (std::uint16_t b = 0; b < books; ++b) {
            ASSERT_EQ(fast.book().top(b, Side::Buy, 100), slow.top(b, Side::Buy));
            ASSERT_EQ(fast.book().top(b, Side::Sell, 100), slow.top(b, Side::Sell));
            const auto bid = fast.book().best(b, Side::Buy);
            const auto ask = fast.book().best(b, Side::Sell);
            if (bid && ask) ASSERT_LT(bid->price, ask->price) << "crossed book after operation " << i;
        }
    }
    // guard against the flow quietly degenerating into something that never trades
    EXPECT_GT(fills, 10'000);
    EXPECT_GT(rejects, 1'000);
}

INSTANTIATE_TEST_SUITE_P(Seeds, EngineDifferential, ::testing::Values(11, 12, 13, 14, 15, 16));

}  // namespace
