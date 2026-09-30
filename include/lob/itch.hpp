#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "lob/types.hpp"

// NASDAQ TotalView-ITCH 5.0 as it appears in the sample files NASDAQ publishes:
// a flat stream of messages, each one prefixed by a two-byte length.
//
// Every integer is big-endian and nothing is aligned, so fields are read with
// memcpy and swapped. Nothing is copied out of the buffer except the fields a
// handler actually asks for.
namespace lob::itch {

static_assert(std::endian::native == std::endian::little, "byte swaps below assume a little-endian host");

namespace detail {

template <class T>
inline T load(const std::byte* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

inline std::uint16_t be16(const std::byte* p) { return __builtin_bswap16(load<std::uint16_t>(p)); }
inline std::uint32_t be32(const std::byte* p) { return __builtin_bswap32(load<std::uint32_t>(p)); }
inline std::uint64_t be64(const std::byte* p) { return __builtin_bswap64(load<std::uint64_t>(p)); }

// Timestamps are six bytes: nanoseconds since midnight.
inline std::uint64_t be48(const std::byte* p) {
    return (std::uint64_t{be16(p)} << 32) | be32(p + 2);
}

inline Side side(const std::byte* p) { return static_cast<char>(*p) == 'B' ? Side::Buy : Side::Sell; }

inline bool has_order_ref(char type) {
    switch (type) {
    case 'A': case 'F': case 'E': case 'C': case 'X': case 'D': case 'U':
        return true;
    default:
        return false;
    }
}

}  // namespace detail

struct SystemEvent {
    std::uint64_t timestamp;
    char code;  // 'O' first message, 'Q' market open, 'M' market close, 'C' last message
};

struct StockDirectory {
    std::uint16_t locate;
    std::string_view symbol;  // eight characters, space padded, points into the input
};

struct AddOrder {
    std::uint16_t locate;
    std::uint64_t timestamp;
    OrderId ref;
    Side side;
    Qty shares;
    Price price;
};

struct OrderExecuted {
    std::uint16_t locate;
    std::uint64_t timestamp;
    OrderId ref;
    Qty shares;
    std::uint64_t match;
};

// Same as OrderExecuted but the trade printed at a price other than the
// order's own (crosses, mostly).
struct OrderExecutedWithPrice {
    std::uint16_t locate;
    std::uint64_t timestamp;
    OrderId ref;
    Qty shares;
    std::uint64_t match;
    bool printable;
    Price price;
};

struct OrderCancel {
    std::uint16_t locate;
    std::uint64_t timestamp;
    OrderId ref;
    Qty shares;
};

struct OrderDelete {
    std::uint16_t locate;
    std::uint64_t timestamp;
    OrderId ref;
};

struct OrderReplace {
    std::uint16_t locate;
    std::uint64_t timestamp;
    OrderId old_ref;
    OrderId new_ref;
    Qty shares;
    Price price;
};

// An execution against an order that was never displayed. It does not change
// the visible book.
struct Trade {
    std::uint16_t locate;
    std::uint64_t timestamp;
    Side side;
    Qty shares;
    Price price;
    std::uint64_t match;
};

// Derive from this and shadow whichever callbacks you need. The calls are
// resolved at compile time, so an empty callback costs nothing.
struct Handler {
    void on_system_event(const SystemEvent&) {}
    void on_stock_directory(const StockDirectory&) {}
    void on_add(const AddOrder&) {}
    void on_executed(const OrderExecuted&) {}
    void on_executed_with_price(const OrderExecutedWithPrice&) {}
    void on_cancel(const OrderCancel&) {}
    void on_delete(const OrderDelete&) {}
    void on_replace(const OrderReplace&) {}
    void on_trade(const Trade&) {}
    void on_other(char /*type*/) {}

    // Called with the order id of a message that is still `lookahead`
    // messages away (see parse). Only useful for prefetching.
    void on_upcoming(OrderId /*ref*/) {}
};

namespace detail {

template <class H>
inline void dispatch(const std::byte* m, std::size_t len, H& h) {
    // Common prefix: type(1) locate(2) tracking(2) timestamp(6), then the body at offset 11.
    const char type = static_cast<char>(m[0]);
    switch (type) {
    case 'A':
    case 'F':  // 'F' is 'A' with a four-byte market participant id on the end
        if (len < 36) break;
        h.on_add(AddOrder{be16(m + 1), be48(m + 5), be64(m + 11), side(m + 19), be32(m + 20), be32(m + 32)});
        return;
    case 'E':
        if (len < 31) break;
        h.on_executed(OrderExecuted{be16(m + 1), be48(m + 5), be64(m + 11), be32(m + 19), be64(m + 23)});
        return;
    case 'C':
        if (len < 36) break;
        h.on_executed_with_price(OrderExecutedWithPrice{be16(m + 1), be48(m + 5), be64(m + 11), be32(m + 19),
                                                        be64(m + 23), static_cast<char>(m[31]) == 'Y',
                                                        be32(m + 32)});
        return;
    case 'X':
        if (len < 23) break;
        h.on_cancel(OrderCancel{be16(m + 1), be48(m + 5), be64(m + 11), be32(m + 19)});
        return;
    case 'D':
        if (len < 19) break;
        h.on_delete(OrderDelete{be16(m + 1), be48(m + 5), be64(m + 11)});
        return;
    case 'U':
        if (len < 35) break;
        h.on_replace(OrderReplace{be16(m + 1), be48(m + 5), be64(m + 11), be64(m + 19), be32(m + 27), be32(m + 31)});
        return;
    case 'P':
        if (len < 44) break;
        h.on_trade(Trade{be16(m + 1), be48(m + 5), side(m + 19), be32(m + 20), be32(m + 32), be64(m + 36)});
        return;
    case 'S':
        if (len < 12) break;
        h.on_system_event(SystemEvent{be48(m + 5), static_cast<char>(m[11])});
        return;
    case 'R':
        if (len < 39) break;
        h.on_stock_directory(StockDirectory{be16(m + 1), {reinterpret_cast<const char*>(m + 11), 8}});
        return;
    default:
        break;
    }
    h.on_other(type);
}

}  // namespace detail

// Calls the handler once per message and returns how many bytes were consumed.
// A return value short of buf.size() means the buffer ended mid-message.
//
// With lookahead > 0 a second cursor runs that many messages in front and
// reports each order id it passes through on_upcoming(). Order lookups are
// cache misses almost by definition, and knowing the id a few messages early
// lets the handler start the miss before it has to wait on it.
template <class H>
std::size_t parse(std::span<const std::byte> buf, H& handler, std::size_t lookahead = 0) {
    const std::byte* p = buf.data();
    const std::byte* const end = p + buf.size();

    const std::byte* ahead = p;
    auto peek = [&] {
        if (end - ahead < 2) return;
        const std::size_t len = detail::be16(ahead);
        if (static_cast<std::size_t>(end - ahead) < 2 + len) {
            ahead = end;
            return;
        }
        // in every order message the id sits right after the common prefix
        if (len >= 19 && detail::has_order_ref(static_cast<char>(ahead[2]))) {
            handler.on_upcoming(detail::be64(ahead + 2 + 11));
        }
        ahead += 2 + len;
    };
    for (std::size_t i = 0; i < lookahead; ++i) peek();

    while (end - p >= 2) {
        const std::size_t len = detail::be16(p);
        if (static_cast<std::size_t>(end - p) < 2 + len) break;
        if (lookahead > 0) peek();
        if (len > 0) detail::dispatch(p + 2, len, handler);
        p += 2 + len;
    }
    return static_cast<std::size_t>(p - buf.data());
}

}  // namespace lob::itch
