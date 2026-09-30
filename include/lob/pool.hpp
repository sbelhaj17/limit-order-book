#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lob {

inline constexpr std::uint32_t kNil = 0xffffffff;

// Hands out slots from one contiguous vector and recycles them. Callers hold
// 32-bit indices rather than pointers, so the vector is free to grow and the
// links inside an order stay small.
template <class T>
class Pool {
public:
    explicit Pool(std::size_t reserve = 0) { items_.reserve(reserve); }

    std::uint32_t alloc() {
        if (!free_.empty()) {
            const std::uint32_t i = free_.back();
            free_.pop_back();
            return i;
        }
        items_.emplace_back();
        return static_cast<std::uint32_t>(items_.size() - 1);
    }

    void release(std::uint32_t i) { free_.push_back(i); }

    T& operator[](std::uint32_t i) { return items_[i]; }
    const T& operator[](std::uint32_t i) const { return items_[i]; }

    std::size_t in_use() const { return items_.size() - free_.size(); }

private:
    std::vector<T> items_;
    std::vector<std::uint32_t> free_;  // the slot freed last is reused first, while it is still in cache
};

}  // namespace lob
