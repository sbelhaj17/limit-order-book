#pragma once

#include <cstdint>

namespace lob {

using OrderId = std::uint64_t;
using Qty = std::uint32_t;

// Prices are integer ticks. ITCH quotes in 1/10000 of a dollar, so $187.25 is 1872500.
using Price = std::uint32_t;

enum class Side : std::uint8_t { Buy, Sell };

constexpr Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

}  // namespace lob
