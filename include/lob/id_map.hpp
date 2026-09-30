#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/pool.hpp"
#include "lob/types.hpp"

namespace lob {

// Order id -> pool index.
//
// Open addressing with linear probing over a power-of-two table. Erasing
// shifts the rest of the run back over the hole instead of leaving a
// tombstone, which matters here because almost every order that is added is
// later removed: a tombstone table would fill up with junk within minutes of
// the open.
class IdMap {
public:
    explicit IdMap(std::size_t expected = 1 << 16) {
        std::size_t capacity = 16;
        while (capacity < expected * 2) capacity *= 2;
        allocate(capacity);
    }

    // kNil if the id is not present.
    std::uint32_t find(OrderId id) const {
        for (std::size_t i = home(id);; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (s.value == kNil) return kNil;
            if (s.key == id) return s.value;
        }
    }

    // Returns false and changes nothing if the id is already there.
    bool insert(OrderId id, std::uint32_t value) {
        if ((size_ + 1) * 2 > slots_.size()) grow();
        for (std::size_t i = home(id);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.value == kNil) {
                s = Slot{id, value};
                ++size_;
                return true;
            }
            if (s.key == id) return false;
        }
    }

    bool erase(OrderId id) {
        std::size_t hole = home(id);
        for (;; hole = (hole + 1) & mask_) {
            if (slots_[hole].value == kNil) return false;
            if (slots_[hole].key == id) break;
        }
        // Walk the rest of the run. An entry may move back into the hole only
        // if that does not put it in front of its own home slot.
        for (std::size_t j = (hole + 1) & mask_; slots_[j].value != kNil; j = (j + 1) & mask_) {
            const std::size_t from_home = (j - home(slots_[j].key)) & mask_;
            const std::size_t from_hole = (j - hole) & mask_;
            if (from_home >= from_hole) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].value = kNil;
        --size_;
        return true;
    }

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return slots_.size(); }

private:
    struct Slot {
        OrderId key;
        std::uint32_t value;  // kNil marks an empty slot
    };

    // ITCH ids are close to sequential, so they need mixing before the top
    // bits are any use. Multiplying by 2^64 / phi does that in one instruction.
    std::size_t home(OrderId id) const { return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> shift_); }

    void allocate(std::size_t capacity) {
        slots_.assign(capacity, Slot{0, kNil});
        mask_ = capacity - 1;
        shift_ = 64;
        for (std::size_t c = capacity; c > 1; c /= 2) --shift_;
        size_ = 0;
    }

    void grow() {
        std::vector<Slot> old = std::move(slots_);
        allocate(old.size() * 2);
        for (const Slot& s : old) {
            if (s.value != kNil) insert(s.key, s.value);
        }
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    unsigned shift_ = 64;
    std::size_t size_ = 0;
};

}  // namespace lob
