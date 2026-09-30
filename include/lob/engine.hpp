#pragma once

#include <cstddef>
#include <limits>

#include "lob/market.hpp"
#include "lob/types.hpp"

namespace lob {

enum class TimeInForce : std::uint8_t {
    Day,  // whatever does not trade rests on the book
    IOC,  // whatever does not trade is cancelled
};

enum class Reject : std::uint8_t { DuplicateId, UnknownId, ZeroQuantity, UnknownInstrument };

struct Fill {
    OrderId resting;
    OrderId incoming;
    Price price;  // always the resting order's price
    Qty qty;
};

// A listener that ignores everything. Derive from it and shadow what you need,
// the same way as itch::Handler.
struct Listener {
    void on_rest(OrderId, Qty /*resting*/) {}
    void on_fill(const Fill&) {}
    void on_cancel(OrderId, Qty /*cancelled*/) {}
    void on_reject(OrderId, Reject) {}
};

// Price-time priority matching on top of Market.
//
// An id identifies an order while it rests. Ids of orders that are gone may be
// reused; an id that is on the book right now may not.
template <class L>
class Engine {
public:
    Engine(std::size_t instruments, L& listener, std::size_t expected_orders = 1 << 20)
        : market_(instruments, expected_orders), listener_(listener) {}

    void limit(std::uint16_t book, OrderId id, Side side, Price price, Qty qty,
               TimeInForce tif = TimeInForce::Day) {
        if (!valid(book, id, qty)) return;

        const Qty left = qty - match(book, id, side, price, qty);
        if (left == 0) return;
        if (tif == TimeInForce::IOC) {
            listener_.on_cancel(id, left);
            return;
        }
        market_.add(book, id, side, price, left);
        listener_.on_rest(id, left);
    }

    // Takes whatever is there, at any price. Never rests.
    void market(std::uint16_t book, OrderId id, Side side, Qty qty) {
        if (!valid(book, id, qty)) return;
        const Price any = side == Side::Buy ? std::numeric_limits<Price>::max() : 0;
        const Qty left = qty - match(book, id, side, any, qty);
        if (left > 0) listener_.on_cancel(id, left);
    }

    void cancel(OrderId id) {
        const auto qty = market_.quantity(id);
        if (!qty) {
            listener_.on_reject(id, Reject::UnknownId);
            return;
        }
        market_.remove(id);
        listener_.on_cancel(id, *qty);
    }

    // Shrinks a resting order in place. It keeps its position in the queue,
    // which is the whole point of amending down instead of replacing.
    void reduce(OrderId id, Qty by) {
        const auto qty = market_.quantity(id);
        if (!qty) {
            listener_.on_reject(id, Reject::UnknownId);
            return;
        }
        if (by == 0) return;
        market_.reduce(id, by);
        listener_.on_cancel(id, std::min(by, *qty));
    }

    // Cancel and re-enter with a new price and size. The new order goes to the
    // back of the queue and trades straight away if the new price crosses.
    void replace(OrderId old_id, OrderId new_id, Price price, Qty qty) {
        const auto old = market_.order(old_id);
        if (!old) {
            listener_.on_reject(new_id, Reject::UnknownId);
            return;
        }
        if (qty == 0) {
            listener_.on_reject(new_id, Reject::ZeroQuantity);
            return;
        }
        if (new_id != old_id && market_.quantity(new_id)) {
            listener_.on_reject(new_id, Reject::DuplicateId);
            return;
        }
        market_.remove(old_id);
        listener_.on_cancel(old_id, old->qty);
        limit(old->book, new_id, old->side, price, qty);
    }

    const Market& book() const { return market_; }

private:
    bool valid(std::uint16_t book, OrderId id, Qty qty) {
        if (book >= market_.instruments()) {
            listener_.on_reject(id, Reject::UnknownInstrument);
            return false;
        }
        if (qty == 0) {
            listener_.on_reject(id, Reject::ZeroQuantity);
            return false;
        }
        if (market_.quantity(id)) {
            listener_.on_reject(id, Reject::DuplicateId);
            return false;
        }
        return true;
    }

    Qty match(std::uint16_t book, OrderId id, Side side, Price limit, Qty qty) {
        return market_.match(book, opposite(side), limit, qty, [&](OrderId resting, Price price, Qty filled) {
            listener_.on_fill(Fill{resting, id, price, filled});
        });
    }

    Market market_;
    L& listener_;
};

}  // namespace lob
