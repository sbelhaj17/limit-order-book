#include "lob/market.hpp"

#include <gtest/gtest.h>

#include "reference_market.hpp"

using namespace lob;

namespace {

constexpr std::uint16_t kBook = 0;

// Every test runs against the real book and against the reference, which
// keeps the two from drifting apart in behaviour.
template <class M>
class MarketTest : public ::testing::Test {};

using Implementations = ::testing::Types<Market, reference::Market>;
TYPED_TEST_SUITE(MarketTest, Implementations);

TYPED_TEST(MarketTest, EmptyBookHasNoQuotes) {
    TypeParam m(1);
    EXPECT_FALSE(m.best(kBook, Side::Buy));
    EXPECT_FALSE(m.best(kBook, Side::Sell));
    EXPECT_EQ(m.live_orders(), 0u);
}

TYPED_TEST(MarketTest, BestBidIsHighestAndBestAskIsLowest) {
    TypeParam m(1);
    m.add(kBook, 1, Side::Buy, 9900, 100);
    m.add(kBook, 2, Side::Buy, 9950, 200);
    m.add(kBook, 3, Side::Buy, 9800, 300);
    m.add(kBook, 4, Side::Sell, 10100, 50);
    m.add(kBook, 5, Side::Sell, 10050, 75);

    auto bid = m.best(kBook, Side::Buy);
    auto ask = m.best(kBook, Side::Sell);
    ASSERT_TRUE(bid && ask);
    EXPECT_EQ(bid->price, 9950u);
    EXPECT_EQ(bid->qty, 200u);
    EXPECT_EQ(ask->price, 10050u);
    EXPECT_EQ(ask->qty, 75u);
    EXPECT_EQ(m.depth(kBook, Side::Buy), 3u);
    EXPECT_EQ(m.depth(kBook, Side::Sell), 2u);
}

TYPED_TEST(MarketTest, OrdersAtOnePriceAddUp) {
    TypeParam m(1);
    m.add(kBook, 1, Side::Sell, 10000, 100);
    m.add(kBook, 2, Side::Sell, 10000, 250);
    auto ask = m.best(kBook, Side::Sell);
    ASSERT_TRUE(ask);
    EXPECT_EQ(ask->qty, 350u);
    EXPECT_EQ(ask->orders, 2u);
    EXPECT_EQ(m.depth(kBook, Side::Sell), 1u);
}

TYPED_TEST(MarketTest, QueueIsFirstInFirstOut) {
    TypeParam m(1);
    m.add(kBook, 1, Side::Buy, 10000, 100);
    m.add(kBook, 2, Side::Buy, 10000, 100);
    m.add(kBook, 3, Side::Buy, 10000, 100);
    EXPECT_TRUE(m.is_next_to_trade(1));
    EXPECT_FALSE(m.is_next_to_trade(2));

    m.remove(1);
    EXPECT_TRUE(m.is_next_to_trade(2));
    EXPECT_FALSE(m.is_next_to_trade(3));
}

TYPED_TEST(MarketTest, QueueListsOrdersInPriority) {
    TypeParam m(1);
    m.add(kBook, 7, Side::Sell, 10000, 100);
    m.add(kBook, 3, Side::Sell, 10000, 100);
    m.add(kBook, 9, Side::Sell, 10000, 100);
    m.remove(3);
    m.add(kBook, 4, Side::Sell, 10000, 100);
    EXPECT_EQ(m.queue(kBook, Side::Sell, 10000), (std::vector<OrderId>{7, 9, 4}));
    EXPECT_TRUE(m.queue(kBook, Side::Sell, 10010).empty());
    EXPECT_TRUE(m.queue(kBook, Side::Buy, 10000).empty());
}

TYPED_TEST(MarketTest, RankByIdSlotsAnOrderAmongItsNeighbours) {
    TypeParam m(1);
    m.add(kBook, 50, Side::Buy, 10000, 100);
    m.add(kBook, 90, Side::Buy, 10000, 100);

    m.add(kBook, 70, Side::Buy, 10000, 100, Rank::Id);
    EXPECT_EQ(m.queue(kBook, Side::Buy, 10000), (std::vector<OrderId>{50, 70, 90}));

    m.add(kBook, 10, Side::Buy, 10000, 100, Rank::Id);  // older than everything
    EXPECT_EQ(m.queue(kBook, Side::Buy, 10000), (std::vector<OrderId>{10, 50, 70, 90}));
    EXPECT_TRUE(m.is_next_to_trade(10));

    m.add(kBook, 99, Side::Buy, 10000, 100, Rank::Id);  // newer than everything
    EXPECT_EQ(m.queue(kBook, Side::Buy, 10000), (std::vector<OrderId>{10, 50, 70, 90, 99}));

    // by arrival the id does not matter
    m.add(kBook, 5, Side::Buy, 10000, 100);
    EXPECT_EQ(m.queue(kBook, Side::Buy, 10000).back(), 5u);
    EXPECT_EQ(m.best(kBook, Side::Buy)->qty, 600u);

    // taking orders out from the front, middle and back leaves the rest in order
    m.remove(10);
    m.remove(70);
    m.remove(5);
    EXPECT_EQ(m.queue(kBook, Side::Buy, 10000), (std::vector<OrderId>{50, 90, 99}));
}

TYPED_TEST(MarketTest, ReplaceCanRankById) {
    TypeParam m(1);
    m.add(kBook, 20, Side::Sell, 10000, 100);
    m.add(kBook, 40, Side::Sell, 10000, 100);
    m.add(kBook, 30, Side::Sell, 10010, 100);
    ASSERT_TRUE(m.replace(30, 35, 10000, 100, Rank::Id));
    EXPECT_EQ(m.queue(kBook, Side::Sell, 10000), (std::vector<OrderId>{20, 35, 40}));
}

TYPED_TEST(MarketTest, OnlyTheBestPriceIsNextToTrade) {
    TypeParam m(1);
    m.add(kBook, 1, Side::Sell, 10010, 100);
    m.add(kBook, 2, Side::Sell, 10000, 100);
    EXPECT_FALSE(m.is_next_to_trade(1));
    EXPECT_TRUE(m.is_next_to_trade(2));
}

TYPED_TEST(MarketTest, PartialFillKeepsQueuePosition) {
    TypeParam m(1);
    m.add(kBook, 1, Side::Buy, 10000, 100);
    m.add(kBook, 2, Side::Buy, 10000, 100);

    ASSERT_TRUE(m.reduce(1, 40));
    EXPECT_EQ(m.quantity(1), 60u);
    EXPECT_TRUE(m.is_next_to_trade(1));
    EXPECT_EQ(m.best(kBook, Side::Buy)->qty, 160u);
}

TYPED_TEST(MarketTest, FillingAnOrderRemovesIt) {
    TypeParam m(1);
    m.add(kBook, 1, Side::Buy, 10000, 100);
    m.add(kBook, 2, Side::Buy, 9990, 100);

    ASSERT_TRUE(m.reduce(1, 100));
    EXPECT_FALSE(m.quantity(1));
    EXPECT_EQ(m.live_orders(), 1u);
    // the 10000 level is gone, 9990 is the new best
    EXPECT_EQ(m.best(kBook, Side::Buy)->price, 9990u);
    EXPECT_EQ(m.depth(kBook, Side::Buy), 1u);
}

TYPED_TEST(MarketTest, ReplaceLosesPriority) {
    TypeParam m(1);
    m.add(kBook, 1, Side::Sell, 10000, 100);
    m.add(kBook, 2, Side::Sell, 10000, 100);

    // same price, new size: still goes to the back
    ASSERT_TRUE(m.replace(1, 3, 10000, 500));
    EXPECT_FALSE(m.quantity(1));
    EXPECT_TRUE(m.is_next_to_trade(2));
    EXPECT_FALSE(m.is_next_to_trade(3));
    EXPECT_EQ(m.best(kBook, Side::Sell)->qty, 600u);
}

TYPED_TEST(MarketTest, ReplaceCanMoveThePrice) {
    TypeParam m(1);
    m.add(kBook, 1, Side::Buy, 10000, 100);
    ASSERT_TRUE(m.replace(1, 2, 10020, 100));
    auto bid = m.best(kBook, Side::Buy);
    EXPECT_EQ(bid->price, 10020u);
    EXPECT_EQ(m.depth(kBook, Side::Buy), 1u);
}

TYPED_TEST(MarketTest, UnknownAndDuplicateIdsAreRejected) {
    TypeParam m(1);
    EXPECT_TRUE(m.add(kBook, 1, Side::Buy, 10000, 100));
    EXPECT_FALSE(m.add(kBook, 1, Side::Buy, 10010, 100));
    EXPECT_FALSE(m.reduce(99, 10));
    EXPECT_FALSE(m.remove(99));
    EXPECT_FALSE(m.replace(99, 100, 10000, 10));
    EXPECT_EQ(m.live_orders(), 1u);
    EXPECT_EQ(m.best(kBook, Side::Buy)->price, 10000u);
}

TYPED_TEST(MarketTest, InstrumentsDoNotSeeEachOther) {
    TypeParam m(3);
    m.add(0, 1, Side::Buy, 10000, 100);
    m.add(2, 2, Side::Buy, 20000, 100);
    EXPECT_EQ(m.best(0, Side::Buy)->price, 10000u);
    EXPECT_FALSE(m.best(1, Side::Buy));
    EXPECT_EQ(m.best(2, Side::Buy)->price, 20000u);

    m.remove(1);
    EXPECT_FALSE(m.best(0, Side::Buy));
    EXPECT_EQ(m.best(2, Side::Buy)->price, 20000u);
}

}  // namespace
