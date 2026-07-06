#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
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
    std::uint16_t locate;
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

    std::uint64_t live_orders() const {
        std::uint64_t n = 0;
        for (const auto& p : pages_)
            if (p) n += p->live;
        return n;
    }

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

class PooledOrderStore {
    static constexpr unsigned kPageBits = 16;
    static constexpr std::size_t kPageSize = std::size_t{1} << kPageBits;
    static constexpr std::uint64_t kPageMask = kPageSize - 1;

    struct Page {
        std::array<Handle, kPageSize> slots;
        std::uint32_t live = 0;
    };

  public:
    Order* find(std::uint64_t ref) {
        const std::size_t pi = ref >> kPageBits;
        if (pi >= pages_.size() || !pages_[pi]) return nullptr;
        const Handle h = pages_[pi]->slots[ref & kPageMask];
        return h == kNil ? nullptr : &pool_[h];
    }

    const Order* find(std::uint64_t ref) const {
        const std::size_t pi = ref >> kPageBits;
        if (pi >= pages_.size() || !pages_[pi]) return nullptr;
        const Handle h = pages_[pi]->slots[ref & kPageMask];
        return h == kNil ? nullptr : &pool_[h];
    }

    Order& touch(std::uint64_t ref) {
        const std::size_t pi = ref >> kPageBits;
        if (pi >= pages_.size()) pages_.resize(pi + 1);
        if (!pages_[pi]) pages_[pi] = take_page();
        Handle& h = pages_[pi]->slots[ref & kPageMask];
        if (h == kNil) {
            if (!free_.empty()) {
                h = free_.back();
                free_.pop_back();
            } else {
                h = static_cast<Handle>(pool_.size());
                pool_.emplace_back();
            }
            pool_[h].level = kNil;
        }
        return pool_[h];
    }

    void note_live(std::uint64_t ref) { ++pages_[ref >> kPageBits]->live; }

    void note_dead(std::uint64_t ref) {
        auto& p = pages_[ref >> kPageBits];
        Handle& h = p->slots[ref & kPageMask];
        free_.push_back(h);
        h = kNil;
        if (--p->live == 0) spare_.push_back(std::move(p));
    }

    std::uint64_t live_orders() const { return pool_.size() - free_.size(); }
    std::size_t pool_capacity() const { return pool_.size(); }

    std::size_t resident_pages() const {
        std::size_t n = 0;
        for (const auto& p : pages_)
            if (p) ++n;
        return n;
    }

    std::size_t spare_pages() const { return spare_.size(); }

  private:
    std::unique_ptr<Page> take_page() {
        std::unique_ptr<Page> p;
        if (!spare_.empty()) {
            p = std::move(spare_.back());
            spare_.pop_back();
        } else {
            p = std::make_unique<Page>();
        }
        p->slots.fill(kNil);
        p->live = 0;
        return p;
    }

    std::vector<std::unique_ptr<Page>> pages_;
    std::vector<std::unique_ptr<Page>> spare_;
    std::vector<Order> pool_;
    std::vector<Handle> free_;
};

class HashOrderStore {
  public:
    HashOrderStore() { map_.reserve(std::size_t{1} << 22); }

    Order* find(std::uint64_t ref) {
        const auto it = map_.find(ref);
        return it == map_.end() || it->second.level == kNil ? nullptr : &it->second;
    }

    const Order* find(std::uint64_t ref) const {
        const auto it = map_.find(ref);
        return it == map_.end() || it->second.level == kNil ? nullptr : &it->second;
    }

    Order& touch(std::uint64_t ref) { return map_[ref]; }

    void note_live(std::uint64_t) {}

    void note_dead(std::uint64_t ref) { map_.erase(ref); }

    std::uint64_t live_orders() const { return map_.size(); }

  private:
    std::unordered_map<std::uint64_t, Order> map_;
};

}  // namespace itch
