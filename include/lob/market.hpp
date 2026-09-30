#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/types.hpp"

namespace lob {

struct Quote {
    Price price;
    Qty qty;        // total resting at this price
    std::size_t orders;
};

// All the books for one trading day, keyed by a small integer per instrument
// (ITCH calls it the stock locate). Order ids are unique across the whole market.
//
// First version: the textbook layout. One std::map per side, a std::list of
// orders per price level, and a hash map from order id to where the order lives.
class Market {
public:
    explicit Market(std::size_t instruments) : books_(instruments) {}

    bool add(std::uint16_t book, OrderId id, Side side, Price price, Qty qty) {
        auto [it, inserted] = orders_.try_emplace(id);
        if (!inserted) return false;

        Level& level = level_for(books_[book], side, price);
        level.queue.push_back(id);
        level.total += qty;

        it->second = Order{qty, price, side, book, std::prev(level.queue.end())};
        return true;
    }

    // An execution or a partial cancel. The order keeps its place in the queue
    // and disappears once nothing is left.
    bool reduce(OrderId id, Qty qty) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return false;
        Order& o = it->second;
        if (qty >= o.qty) {
            unlink(it);
            return true;
        }
        o.qty -= qty;
        level_for(books_[o.book], o.side, o.price).total -= qty;
        return true;
    }

    bool remove(OrderId id) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return false;
        unlink(it);
        return true;
    }

    // The replacement gets a new id and goes to the back of the queue at its
    // new price, same side and instrument as the order it replaces.
    bool replace(OrderId old_id, OrderId new_id, Price price, Qty qty) {
        auto it = orders_.find(old_id);
        if (it == orders_.end()) return false;
        const Side side = it->second.side;
        const std::uint16_t book = it->second.book;
        unlink(it);
        return add(book, new_id, side, price, qty);
    }

    std::optional<Quote> best(std::uint16_t book, Side side) const {
        const Book& b = books_[book];
        if (side == Side::Buy) {
            if (b.bids.empty()) return std::nullopt;
            const auto& [price, level] = *b.bids.begin();
            return Quote{price, level.total, level.queue.size()};
        }
        if (b.asks.empty()) return std::nullopt;
        const auto& [price, level] = *b.asks.begin();
        return Quote{price, level.total, level.queue.size()};
    }

    // The n best price levels on one side, best first.
    std::vector<Quote> top(std::uint16_t book, Side side, std::size_t n) const {
        std::vector<Quote> out;
        auto collect = [&](const auto& levels) {
            for (const auto& [price, level] : levels) {
                if (out.size() == n) break;
                out.push_back(Quote{price, level.total, level.queue.size()});
            }
        };
        if (side == Side::Buy) collect(books_[book].bids);
        else collect(books_[book].asks);
        return out;
    }

    // True if this order is the next one to trade on its side: best price,
    // front of the queue.
    bool is_next_to_trade(OrderId id) const {
        auto it = orders_.find(id);
        if (it == orders_.end()) return false;
        const Order& o = it->second;
        const Book& b = books_[o.book];
        if (o.side == Side::Buy) {
            const auto& [price, level] = *b.bids.begin();
            return price == o.price && level.queue.front() == id;
        }
        const auto& [price, level] = *b.asks.begin();
        return price == o.price && level.queue.front() == id;
    }

    std::optional<Qty> quantity(OrderId id) const {
        auto it = orders_.find(id);
        if (it == orders_.end()) return std::nullopt;
        return it->second.qty;
    }

    std::size_t depth(std::uint16_t book, Side side) const {
        return side == Side::Buy ? books_[book].bids.size() : books_[book].asks.size();
    }

    std::size_t live_orders() const { return orders_.size(); }
    std::size_t instruments() const { return books_.size(); }

private:
    struct Level {
        Qty total = 0;
        std::list<OrderId> queue;
    };

    struct Book {
        std::map<Price, Level, std::greater<>> bids;  // best (highest) first
        std::map<Price, Level> asks;                  // best (lowest) first
    };

    struct Order {
        Qty qty;
        Price price;
        Side side;
        std::uint16_t book;
        std::list<OrderId>::iterator pos;
    };

    static Level& level_for(Book& b, Side side, Price price) {
        return side == Side::Buy ? b.bids[price] : b.asks[price];
    }

    void unlink(std::unordered_map<OrderId, Order>::iterator it) {
        const Order& o = it->second;
        Book& b = books_[o.book];
        Level& level = level_for(b, o.side, o.price);
        level.total -= o.qty;
        level.queue.erase(o.pos);
        if (level.queue.empty()) {
            if (o.side == Side::Buy) b.bids.erase(o.price);
            else b.asks.erase(o.price);
        }
        orders_.erase(it);
    }

    std::vector<Book> books_;
    std::unordered_map<OrderId, Order> orders_;
};

}  // namespace lob
