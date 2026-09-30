#include "lob/id_map.hpp"

#include <gtest/gtest.h>

#include <random>
#include <unordered_map>
#include <vector>

using namespace lob;

namespace {

TEST(IdMap, InsertFindErase) {
    IdMap m;
    EXPECT_EQ(m.find(42), kNil);
    EXPECT_TRUE(m.insert(42, 7));
    EXPECT_EQ(m.find(42), 7u);
    EXPECT_EQ(m.size(), 1u);

    EXPECT_FALSE(m.insert(42, 8));  // already there, value untouched
    EXPECT_EQ(m.find(42), 7u);

    EXPECT_TRUE(m.erase(42));
    EXPECT_EQ(m.find(42), kNil);
    EXPECT_FALSE(m.erase(42));
    EXPECT_EQ(m.size(), 0u);
}

TEST(IdMap, ZeroIsAnOrdinaryKey) {
    IdMap m;
    EXPECT_EQ(m.find(0), kNil);
    EXPECT_TRUE(m.insert(0, 5));
    EXPECT_EQ(m.find(0), 5u);
    EXPECT_TRUE(m.erase(0));
    EXPECT_EQ(m.find(0), kNil);
}

TEST(IdMap, GrowsAndKeepsEverything) {
    IdMap m(4);
    const std::size_t start = m.capacity();
    for (std::uint32_t i = 0; i < 10'000; ++i) ASSERT_TRUE(m.insert(1'000'000 + i, i));
    EXPECT_GT(m.capacity(), start);
    EXPECT_EQ(m.size(), 10'000u);
    for (std::uint32_t i = 0; i < 10'000; ++i) ASSERT_EQ(m.find(1'000'000 + i), i);
}

// The interesting cases for backward-shift deletion are long runs and runs
// that wrap around the end of the table. Holding the map at a handful of
// entries keeps the table at its minimum size, so both happen constantly.
TEST(IdMap, MatchesUnorderedMapInATinyTable) {
    IdMap m(1);
    std::unordered_map<OrderId, std::uint32_t> truth;
    std::mt19937_64 rng(99);
    const std::size_t small = m.capacity();

    for (int i = 0; i < 200'000; ++i) {
        const OrderId id = rng() % 64;
        if (rng() % 2 && truth.size() < small / 2 - 1) {
            const auto value = static_cast<std::uint32_t>(rng() % 1000);
            ASSERT_EQ(m.insert(id, value), truth.try_emplace(id, value).second);
        } else {
            ASSERT_EQ(m.erase(id), truth.erase(id) == 1);
        }
        ASSERT_EQ(m.size(), truth.size());
        for (OrderId k = 0; k < 64; ++k) {
            auto it = truth.find(k);
            ASSERT_EQ(m.find(k), it == truth.end() ? kNil : it->second) << "key " << k << " after step " << i;
        }
    }
    EXPECT_EQ(m.capacity(), small);
}

TEST(IdMap, MatchesUnorderedMapUnderChurn) {
    IdMap m;
    std::unordered_map<OrderId, std::uint32_t> truth;
    std::vector<OrderId> live;
    std::mt19937_64 rng(7);
    OrderId next = 1;

    // roughly the shape of a trading day: ids only go up and most of them are
    // erased again soon after
    for (int i = 0; i < 500'000; ++i) {
        if (live.empty() || rng() % 100 < 52) {
            const OrderId id = next;
            next += 1 + rng() % 3;
            const auto value = static_cast<std::uint32_t>(i);
            ASSERT_TRUE(m.insert(id, value));
            truth.emplace(id, value);
            live.push_back(id);
        } else {
            const std::size_t at = rng() % live.size();
            ASSERT_TRUE(m.erase(live[at]));
            truth.erase(live[at]);
            live[at] = live.back();
            live.pop_back();
        }
    }
    ASSERT_EQ(m.size(), truth.size());
    for (const auto& [id, value] : truth) ASSERT_EQ(m.find(id), value);
    for (OrderId id = 0; id < next; id += 7) {
        auto it = truth.find(id);
        ASSERT_EQ(m.find(id), it == truth.end() ? kNil : it->second);
    }
}

}  // namespace
