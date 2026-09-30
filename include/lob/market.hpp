#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

#include "lob/id_map.hpp"
#include "lob/pool.hpp"
#include "lob/types.hpp"

namespace lob {

// All the books for one trading day, keyed by a small integer per instrument
// (ITCH calls it the stock locate). Order ids are unique across the whole market.
//
// Layout:
//   - orders sit in one pool; each price level threads a doubly linked list
//     through them, so unlinking an order touches its two neighbours only
//   - levels sit in a second pool; each side of each book is a small sorted
//     vector of (price, level) with the best price at the back
//   - an open-addressing table maps order id to pool slot
class Market {
public:
    explicit Market(std::size_t instruments, std::size_t expected_orders = 1 << 20)
        : books_(instruments), orders_(expected_orders), levels_(expected_orders / 8), ids_(expected_orders) {}

    // Rank::Id exists for market data. NASDAQ numbers orders as it receives
    // them and ranks them in that order, but an order entered before the open
    // only shows up in the feed at 9:30, after later orders that were already
    // on the book. Queueing by id instead of by arrival puts it where the
    // exchange has it. For everything else the two rules give the same queue.
    bool add(std::uint16_t book, OrderId id, Side side, Price price, Qty qty, Rank rank = Rank::Arrival) {
        const std::uint32_t o = orders_.alloc();
        if (!ids_.insert(id, o)) {
            orders_.release(o);
            return false;
        }
        const std::uint32_t l = level_for(book, side, price);
        Level& level = levels_[l];

        // The neighbours this order goes between; kNil means the end of the queue.
        std::uint32_t before = level.tail;
        std::uint32_t after = kNil;
        if (rank == Rank::Id) {
            // Nearly always the tail already has a smaller id and this loop
            // does not run. When an old id arrives late, walk back to its place.
            while (before != kNil && orders_[before].id > id) {
                after = before;
                before = orders_[before].prev;
            }
        }

        Order& order = orders_[o];
        order.id = id;
        order.qty = qty;
        order.level = l;
        order.prev = before;
        order.next = after;
        if (before != kNil) orders_[before].next = o;
        else level.head = o;
        if (after != kNil) orders_[after].prev = o;
        else level.tail = o;
        level.total += qty;
        ++level.count;
        return true;
    }

    // An execution or a partial cancel. The order keeps its place in the queue
    // and disappears once nothing is left.
    bool reduce(OrderId id, Qty qty) {
        const std::uint32_t o = ids_.find(id);
        if (o == kNil) return false;
        Order& order = orders_[o];
        if (qty >= order.qty) {
            unlink(o);
            return true;
        }
        order.qty -= qty;
        levels_[order.level].total -= qty;
        return true;
    }

    bool remove(OrderId id) {
        const std::uint32_t o = ids_.find(id);
        if (o == kNil) return false;
        unlink(o);
        return true;
    }

    // The replacement gets a new id and goes to the back of the queue at its
    // new price, same side and instrument as the order it replaces.
    bool replace(OrderId old_id, OrderId new_id, Price price, Qty qty, Rank rank = Rank::Arrival) {
        const std::uint32_t o = ids_.find(old_id);
        if (o == kNil) return false;
        const Level& level = levels_[orders_[o].level];
        const Side side = level.side;
        const std::uint16_t book = level.book;
        unlink(o);
        return add(book, new_id, side, price, qty, rank);
    }

    // Trades up to qty against the resting orders on one side: best price
    // first, oldest order first within a price, and never through the limit.
    // Calls on_fill(resting id, price, quantity) for each order it touches
    // and returns the total filled.
    template <class OnFill>
    Qty match(std::uint16_t book, Side resting, Price limit, Qty qty, OnFill&& on_fill) {
        Ladder& ladder = books_[book].side(resting);
        const std::uint32_t worst = key(resting, limit);
        Qty filled = 0;
        while (filled < qty && !ladder.empty() && ladder.back().key >= worst) {
            const std::uint32_t l = ladder.back().level;
            const std::uint32_t o = levels_[l].head;
            Order& order = orders_[o];
            const Qty take = std::min(qty - filled, order.qty);
            on_fill(order.id, levels_[l].price, take);
            filled += take;
            if (take == order.qty) {
                unlink(o);  // drops the level too if this was its last order
            } else {
                order.qty -= take;
                levels_[l].total -= take;
            }
        }
        return filled;
    }

    // Hint that this id is about to be added or looked up.
    void prefetch(OrderId id) const { ids_.prefetch(id); }

    std::optional<Quote> best(std::uint16_t book, Side side) const {
        const Ladder& ladder = books_[book].side(side);
        if (ladder.empty()) return std::nullopt;
        return quote(ladder.back().level);
    }

    // The n best price levels on one side, best first.
    std::vector<Quote> top(std::uint16_t book, Side side, std::size_t n) const {
        const Ladder& ladder = books_[book].side(side);
        std::vector<Quote> out;
        for (auto it = ladder.rbegin(); it != ladder.rend() && out.size() < n; ++it) out.push_back(quote(it->level));
        return out;
    }

    // Ids resting at one price, in the order they would trade.
    std::vector<OrderId> queue(std::uint16_t book, Side side, Price price) const {
        std::vector<OrderId> out;
        const Ladder& ladder = books_[book].side(side);
        const std::uint32_t k = key(side, price);
        const std::size_t i = position(ladder, k);
        if (i == 0 || ladder[i - 1].key != k) return out;
        for (std::uint32_t o = levels_[ladder[i - 1].level].head; o != kNil; o = orders_[o].next) {
            out.push_back(orders_[o].id);
        }
        return out;
    }

    // True if this order is the next one to trade on its side: best price,
    // front of the queue.
    bool is_next_to_trade(OrderId id) const {
        const std::uint32_t o = ids_.find(id);
        if (o == kNil) return false;
        const std::uint32_t l = orders_[o].level;
        const Level& level = levels_[l];
        return level.head == o && books_[level.book].side(level.side).back().level == l;
    }

    std::optional<Qty> quantity(OrderId id) const {
        const std::uint32_t o = ids_.find(id);
        if (o == kNil) return std::nullopt;
        return orders_[o].qty;
    }

    struct OrderView {
        std::uint16_t book;
        Side side;
        Price price;
        Qty qty;
    };

    std::optional<OrderView> order(OrderId id) const {
        const std::uint32_t o = ids_.find(id);
        if (o == kNil) return std::nullopt;
        const Level& level = levels_[orders_[o].level];
        return OrderView{level.book, level.side, level.price, orders_[o].qty};
    }

    std::size_t depth(std::uint16_t book, Side side) const { return books_[book].side(side).size(); }
    std::size_t live_orders() const { return ids_.size(); }
    std::size_t instruments() const { return books_.size(); }

private:
    struct Order {
        OrderId id;
        std::uint32_t prev;
        std::uint32_t next;
        std::uint32_t level;
        Qty qty;
    };
    static_assert(sizeof(Order) == 24);

    struct Level {
        Price price;
        Qty total;
        std::uint32_t head;
        std::uint32_t tail;
        std::uint32_t count;
        std::uint16_t book;
        Side side;
    };

    struct LevelRef {
        std::uint32_t key;
        std::uint32_t level;
    };

    // One side of one book, ascending by key, so the best price is at the back
    // where inserting and erasing are cheap.
    using Ladder = std::vector<LevelRef>;

    struct Book {
        Ladder bids;
        Ladder asks;

        Ladder& side(Side s) { return s == Side::Buy ? bids : asks; }
        const Ladder& side(Side s) const { return s == Side::Buy ? bids : asks; }
    };

    // Ask prices are flipped so that on both sides a better price is a larger
    // key. Nothing below has to branch on the side after this.
    static std::uint32_t key(Side side, Price price) { return side == Side::Buy ? price : ~price; }

    // How many entries have a key <= k, which is also where k would be
    // inserted. If k is present it is at position - 1.
    //
    // Walks in from the best price because that is where nearly everything
    // happens; a binary search would touch colder memory for no gain.
    static std::size_t position(const Ladder& ladder, std::uint32_t k) {
        std::size_t i = ladder.size();
        while (i > 0 && ladder[i - 1].key > k) --i;
        return i;
    }

    Quote quote(std::uint32_t l) const {
        const Level& level = levels_[l];
        return Quote{level.price, level.total, level.count};
    }

    std::uint32_t level_for(std::uint16_t book, Side side, Price price) {
        Ladder& ladder = books_[book].side(side);
        const std::uint32_t k = key(side, price);
        const std::size_t i = position(ladder, k);
        if (i > 0 && ladder[i - 1].key == k) return ladder[i - 1].level;

        const std::uint32_t l = levels_.alloc();
        levels_[l] = Level{price, 0, kNil, kNil, 0, book, side};
        ladder.insert(ladder.begin() + static_cast<std::ptrdiff_t>(i), LevelRef{k, l});
        return l;
    }

    void unlink(std::uint32_t o) {
        const Order& order = orders_[o];
        const std::uint32_t l = order.level;
        Level& level = levels_[l];

        if (order.prev != kNil) orders_[order.prev].next = order.next;
        else level.head = order.next;
        if (order.next != kNil) orders_[order.next].prev = order.prev;
        else level.tail = order.prev;

        level.total -= order.qty;
        if (--level.count == 0) {
            Ladder& ladder = books_[level.book].side(level.side);
            const std::size_t i = position(ladder, key(level.side, level.price));
            ladder.erase(ladder.begin() + static_cast<std::ptrdiff_t>(i - 1));
            levels_.release(l);
        }
        ids_.erase(order.id);
        orders_.release(o);
    }

    std::vector<Book> books_;
    Pool<Order> orders_;
    Pool<Level> levels_;
    IdMap ids_;
};

}  // namespace lob
