#pragma once

#include <cstddef>
#include <cstdint>

namespace lob {

using OrderId = std::uint64_t;
using Qty = std::uint32_t;

// Prices are integer ticks. ITCH quotes in 1/10000 of a dollar, so $187.25 is 1872500.
using Price = std::uint32_t;

enum class Side : std::uint8_t { Buy, Sell };

constexpr Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

// Where a new order goes in the queue at its price.
enum class Rank : std::uint8_t {
    Arrival,  // behind everything already there
    Id,       // in id order: ahead of any resting order with a larger id
};

// One price level as seen from outside the book.
struct Quote {
    Price price;
    Qty qty;  // total resting at this price
    std::size_t orders;

    friend bool operator==(const Quote&, const Quote&) = default;
};

}  // namespace lob
