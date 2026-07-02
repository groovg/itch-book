#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace itch {

using Handle = std::uint32_t;
inline constexpr Handle kNil = 0xffffffffu;
inline constexpr std::uint64_t kNilRef = 0;

struct Order {
    std::uint64_t prev;
    std::uint64_t next;
    std::uint32_t qty;
    Handle level = kNil;
    std::uint8_t buy;
};

class OrderStore {
    static constexpr unsigned kPageBits = 16;
    static constexpr std::size_t kPageSize = std::size_t{1} << kPageBits;
    static constexpr std::uint64_t kPageMask = kPageSize - 1;

    struct Page {
        std::array<Order, kPageSize> slots;
        std::uint32_t live = 0;
    };

  public:
    Order* find(std::uint64_t ref) {
        const std::size_t pi = ref >> kPageBits;
        if (pi >= pages_.size() || !pages_[pi]) return nullptr;
        Order& o = pages_[pi]->slots[ref & kPageMask];
        return o.level != kNil ? &o : nullptr;
    }

    const Order* find(std::uint64_t ref) const {
        const std::size_t pi = ref >> kPageBits;
        if (pi >= pages_.size() || !pages_[pi]) return nullptr;
        const Order& o = pages_[pi]->slots[ref & kPageMask];
        return o.level != kNil ? &o : nullptr;
    }

    Order& touch(std::uint64_t ref) {
        const std::size_t pi = ref >> kPageBits;
        if (pi >= pages_.size()) pages_.resize(pi + 1);
        if (!pages_[pi]) pages_[pi] = take_page();
        return pages_[pi]->slots[ref & kPageMask];
    }

    void note_live(std::uint64_t ref) { ++pages_[ref >> kPageBits]->live; }

    void note_dead(std::uint64_t ref) {
        auto& p = pages_[ref >> kPageBits];
        if (--p->live == 0) spare_.push_back(std::move(p));
    }

    std::size_t resident_pages() const {
        std::size_t n = 0;
        for (const auto& p : pages_)
            if (p) ++n;
        return n;
    }

    std::size_t spare_pages() const { return spare_.size(); }

  private:
    std::unique_ptr<Page> take_page() {
        if (!spare_.empty()) {
            auto p = std::move(spare_.back());
            spare_.pop_back();
            reset(*p);
            return p;
        }
        return std::make_unique<Page>();
    }

    static void reset(Page& p) {
        for (auto& o : p.slots) o.level = kNil;
        p.live = 0;
    }

    std::vector<std::unique_ptr<Page>> pages_;
    std::vector<std::unique_ptr<Page>> spare_;
};

}  // namespace itch
