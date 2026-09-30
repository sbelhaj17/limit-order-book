#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/pool.hpp"
#include "lob/types.hpp"

namespace lob {

// All the books for one trading day, keyed by a small integer per instrument
// (ITCH calls it the stock locate). Order ids are unique across the whole market.
//
// Orders live in a pool and each price level threads a doubly linked list
// through them, so taking an order out of its queue touches its two neighbours
// and nothing else.
class Market {
public:
    explicit Market(std::size_t instruments, std::size_t expected_orders = 1 << 20)
        : books_(instruments), orders_(expected_orders), levels_(expected_orders / 8) {
        ids_.reserve(expected_orders);
    }

    bool add(std::uint16_t book, OrderId id, Side side, Price price, Qty qty) {
        auto [it, inserted] = ids_.try_emplace(id, kNil);
        if (!inserted) return false;

        const std::uint32_t l = level_for(book, side, price);
        const std::uint32_t o = orders_.alloc();
        it->second = o;

        Level& level = levels_[l];
        Order& order = orders_[o];
        order.id = id;
        order.qty = qty;
        order.level = l;
        order.next = kNil;
        order.prev = level.tail;
        if (level.tail != kNil) orders_[level.tail].next = o;
        else level.head = o;
        level.tail = o;
        level.total += qty;
        ++level.count;
        return true;
    }

    // An execution or a partial cancel. The order keeps its place in the queue
    // and disappears once nothing is left.
    bool reduce(OrderId id, Qty qty) {
        auto it = ids_.find(id);
        if (it == ids_.end()) return false;
        Order& order = orders_[it->second];
        if (qty >= order.qty) {
            unlink(it);
            return true;
        }
        order.qty -= qty;
        levels_[order.level].total -= qty;
        return true;
    }

    bool remove(OrderId id) {
        auto it = ids_.find(id);
        if (it == ids_.end()) return false;
        unlink(it);
        return true;
    }

    // The replacement gets a new id and goes to the back of the queue at its
    // new price, same side and instrument as the order it replaces.
    bool replace(OrderId old_id, OrderId new_id, Price price, Qty qty) {
        auto it = ids_.find(old_id);
        if (it == ids_.end()) return false;
        const Level& level = levels_[orders_[it->second].level];
        const Side side = level.side;
        const std::uint16_t book = level.book;
        unlink(it);
        return add(book, new_id, side, price, qty);
    }

    std::optional<Quote> best(std::uint16_t book, Side side) const {
        const Book& b = books_[book];
        if (side == Side::Buy) {
            if (b.bids.empty()) return std::nullopt;
            return quote(b.bids.begin()->second);
        }
        if (b.asks.empty()) return std::nullopt;
        return quote(b.asks.begin()->second);
    }

    // The n best price levels on one side, best first.
    std::vector<Quote> top(std::uint16_t book, Side side, std::size_t n) const {
        std::vector<Quote> out;
        auto collect = [&](const auto& levels) {
            for (const auto& [price, l] : levels) {
                if (out.size() == n) break;
                out.push_back(quote(l));
            }
        };
        if (side == Side::Buy) collect(books_[book].bids);
        else collect(books_[book].asks);
        return out;
    }

    // Ids resting at one price, in the order they would trade.
    std::vector<OrderId> queue(std::uint16_t book, Side side, Price price) const {
        std::vector<OrderId> out;
        const Book& b = books_[book];
        std::uint32_t l = kNil;
        if (side == Side::Buy) {
            if (auto it = b.bids.find(price); it != b.bids.end()) l = it->second;
        } else {
            if (auto it = b.asks.find(price); it != b.asks.end()) l = it->second;
        }
        if (l == kNil) return out;
        for (std::uint32_t o = levels_[l].head; o != kNil; o = orders_[o].next) out.push_back(orders_[o].id);
        return out;
    }

    // True if this order is the next one to trade on its side: best price,
    // front of the queue.
    bool is_next_to_trade(OrderId id) const {
        auto it = ids_.find(id);
        if (it == ids_.end()) return false;
        const std::uint32_t o = it->second;
        const std::uint32_t l = orders_[o].level;
        const Level& level = levels_[l];
        if (level.head != o) return false;
        const Book& b = books_[level.book];
        return l == (level.side == Side::Buy ? b.bids.begin()->second : b.asks.begin()->second);
    }

    std::optional<Qty> quantity(OrderId id) const {
        auto it = ids_.find(id);
        if (it == ids_.end()) return std::nullopt;
        return orders_[it->second].qty;
    }

    std::size_t depth(std::uint16_t book, Side side) const {
        return side == Side::Buy ? books_[book].bids.size() : books_[book].asks.size();
    }

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

    struct Book {
        std::map<Price, std::uint32_t, std::greater<>> bids;  // best (highest) first
        std::map<Price, std::uint32_t> asks;                  // best (lowest) first
    };

    using IdMap = std::unordered_map<OrderId, std::uint32_t>;

    Quote quote(std::uint32_t l) const {
        const Level& level = levels_[l];
        return Quote{level.price, level.total, level.count};
    }

    std::uint32_t level_for(std::uint16_t book, Side side, Price price) {
        Book& b = books_[book];
        std::uint32_t& slot = side == Side::Buy ? b.bids.try_emplace(price, kNil).first->second
                                                : b.asks.try_emplace(price, kNil).first->second;
        if (slot == kNil) {
            slot = levels_.alloc();
            levels_[slot] = Level{price, 0, kNil, kNil, 0, book, side};
        }
        return slot;
    }

    void unlink(IdMap::iterator it) {
        const std::uint32_t o = it->second;
        const Order& order = orders_[o];
        const std::uint32_t l = order.level;
        Level& level = levels_[l];

        if (order.prev != kNil) orders_[order.prev].next = order.next;
        else level.head = order.next;
        if (order.next != kNil) orders_[order.next].prev = order.prev;
        else level.tail = order.prev;

        level.total -= order.qty;
        if (--level.count == 0) {
            Book& b = books_[level.book];
            if (level.side == Side::Buy) b.bids.erase(level.price);
            else b.asks.erase(level.price);
            levels_.release(l);
        }
        orders_.release(o);
        ids_.erase(it);
    }

    std::vector<Book> books_;
    Pool<Order> orders_;
    Pool<Level> levels_;
    IdMap ids_;
};

}  // namespace lob
