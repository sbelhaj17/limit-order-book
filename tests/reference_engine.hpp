#pragma once

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <unordered_map>
#include <vector>

#include "lob/engine.hpp"
#include "lob/types.hpp"

namespace lob::reference {

// Everything an engine can tell its listener, flattened into one comparable
// record so two engines' output can be checked with ==.
struct Event {
    enum Kind : char { Rest = 'R', Filled = 'F', Cancel = 'C', Rejected = 'X' };

    Kind kind;
    OrderId id;        // the order the event is about (the resting order for a fill)
    OrderId incoming;  // fills only
    Price price;       // fills only
    Qty qty;           // resting, filled or cancelled quantity; the reason for a reject

    friend bool operator==(const Event&, const Event&) = default;
};

// Turns listener callbacks into Events. Used for both engines.
struct Recorder : Listener {
    std::vector<Event> events;

    void on_rest(OrderId id, Qty qty) { events.push_back({Event::Rest, id, 0, 0, qty}); }
    void on_fill(const Fill& f) { events.push_back({Event::Filled, f.resting, f.incoming, f.price, f.qty}); }
    void on_cancel(OrderId id, Qty qty) { events.push_back({Event::Cancel, id, 0, 0, qty}); }
    void on_reject(OrderId id, Reject why) { events.push_back({Event::Rejected, id, 0, 0, static_cast<Qty>(why)}); }
};

// A matching engine written for clarity and nothing else: a map of deques per
// side and no cleverness. It exists so the real engine has something to
// disagree with.
class Engine {
public:
    explicit Engine(std::size_t instruments) : books_(instruments) {}

    std::vector<Event> events;

    void limit(std::uint16_t book, OrderId id, Side side, Price price, Qty qty,
               TimeInForce tif = TimeInForce::Day) {
        if (!valid(book, id, qty)) return;
        qty = match(book, id, side, price, qty);
        if (qty == 0) return;
        if (tif == TimeInForce::IOC) {
            events.push_back({Event::Cancel, id, 0, 0, qty});
            return;
        }
        levels(book, side)[price].push_back({id, qty});
        where_[id] = {book, side, price};
        events.push_back({Event::Rest, id, 0, 0, qty});
    }

    void market(std::uint16_t book, OrderId id, Side side, Qty qty) {
        if (!valid(book, id, qty)) return;
        const Price any = side == Side::Buy ? std::numeric_limits<Price>::max() : 0;
        qty = match(book, id, side, any, qty);
        if (qty > 0) events.push_back({Event::Cancel, id, 0, 0, qty});
    }

    void cancel(OrderId id) {
        auto it = where_.find(id);
        if (it == where_.end()) {
            events.push_back({Event::Rejected, id, 0, 0, static_cast<Qty>(Reject::UnknownId)});
            return;
        }
        events.push_back({Event::Cancel, id, 0, 0, erase(it)});
    }

    void reduce(OrderId id, Qty by) {
        auto it = where_.find(id);
        if (it == where_.end()) {
            events.push_back({Event::Rejected, id, 0, 0, static_cast<Qty>(Reject::UnknownId)});
            return;
        }
        if (by == 0) return;
        Resting& r = resting(it->second, id);
        if (by >= r.qty) {
            events.push_back({Event::Cancel, id, 0, 0, erase(it)});
        } else {
            r.qty -= by;
            events.push_back({Event::Cancel, id, 0, 0, by});
        }
    }

    void replace(OrderId old_id, OrderId new_id, Price price, Qty qty) {
        auto it = where_.find(old_id);
        if (it == where_.end()) {
            events.push_back({Event::Rejected, new_id, 0, 0, static_cast<Qty>(Reject::UnknownId)});
            return;
        }
        if (qty == 0) {
            events.push_back({Event::Rejected, new_id, 0, 0, static_cast<Qty>(Reject::ZeroQuantity)});
            return;
        }
        if (new_id != old_id && where_.count(new_id)) {
            events.push_back({Event::Rejected, new_id, 0, 0, static_cast<Qty>(Reject::DuplicateId)});
            return;
        }
        const Where w = it->second;
        events.push_back({Event::Cancel, old_id, 0, 0, erase(it)});
        limit(w.book, new_id, w.side, price, qty);
    }

    bool resting(OrderId id) const { return where_.count(id) > 0; }

    // total quantity per price on one side, best first
    std::vector<Quote> top(std::uint16_t book, Side side) const {
        std::vector<Quote> out;
        auto push = [&](const auto& entry) {
            Qty total = 0;
            for (const Resting& r : entry.second) total += r.qty;
            out.push_back(Quote{entry.first, total, entry.second.size()});
        };
        const Levels& l = side == Side::Buy ? books_[book].bids : books_[book].asks;
        if (side == Side::Buy) std::for_each(l.rbegin(), l.rend(), push);
        else std::for_each(l.begin(), l.end(), push);
        return out;
    }

private:
    struct Resting {
        OrderId id;
        Qty qty;
    };
    using Levels = std::map<Price, std::deque<Resting>>;
    struct Book {
        Levels bids;
        Levels asks;
    };
    struct Where {
        std::uint16_t book;
        Side side;
        Price price;
    };

    Levels& levels(std::uint16_t book, Side side) { return side == Side::Buy ? books_[book].bids : books_[book].asks; }

    bool valid(std::uint16_t book, OrderId id, Qty qty) {
        Reject why;
        if (book >= books_.size()) why = Reject::UnknownInstrument;
        else if (qty == 0) why = Reject::ZeroQuantity;
        else if (where_.count(id)) why = Reject::DuplicateId;
        else return true;
        events.push_back({Event::Rejected, id, 0, 0, static_cast<Qty>(why)});
        return false;
    }

    // returns what is left of the incoming order
    Qty match(std::uint16_t book, OrderId id, Side side, Price limit, Qty qty) {
        Levels& other = levels(book, opposite(side));
        while (qty > 0 && !other.empty()) {
            auto best = side == Side::Buy ? other.begin() : std::prev(other.end());
            const bool crosses = side == Side::Buy ? best->first <= limit : best->first >= limit;
            if (!crosses) break;
            Resting& r = best->second.front();
            const Qty fill = std::min(qty, r.qty);
            events.push_back({Event::Filled, r.id, id, best->first, fill});
            qty -= fill;
            r.qty -= fill;
            if (r.qty == 0) {
                where_.erase(r.id);
                best->second.pop_front();
                if (best->second.empty()) other.erase(best);
            }
        }
        return qty;
    }

    Resting& resting(const Where& w, OrderId id) {
        auto& queue = levels(w.book, w.side)[w.price];
        return *std::find_if(queue.begin(), queue.end(), [&](const Resting& r) { return r.id == id; });
    }

    // removes the order and returns the quantity it still had
    Qty erase(std::unordered_map<OrderId, Where>::iterator it) {
        const Where w = it->second;
        Levels& l = levels(w.book, w.side);
        auto& queue = l[w.price];
        auto pos = std::find_if(queue.begin(), queue.end(), [&](const Resting& r) { return r.id == it->first; });
        const Qty qty = pos->qty;
        queue.erase(pos);
        if (queue.empty()) l.erase(w.price);
        where_.erase(it);
        return qty;
    }

    std::vector<Book> books_;
    std::unordered_map<OrderId, Where> where_;
};

}  // namespace lob::reference
