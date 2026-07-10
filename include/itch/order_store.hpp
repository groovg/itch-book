#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace itch {

using Handle = std::uint32_t;
inline constexpr Handle kNil = 0xffffffffu;
inline constexpr std::uint64_t kNilRef = 0;
inline constexpr std::uint64_t kMaxRef = std::uint64_t{1} << 36;

struct Order {
    std::uint64_t prev;
    std::uint64_t next;
    std::uint32_t qty;
    Handle level = kNil;
    std::uint16_t locate;
    std::uint8_t buy;
};

class OrderStore {
    static constexpr unsigned kPageBits = 13;
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

    void reserve(std::uint64_t ref_space, std::uint64_t, std::size_t spare_hint) {
        pages_.reserve((ref_space >> kPageBits) + 1);
        spare_.reserve(spare_hint);
        while (spare_.size() < spare_hint) spare_.push_back(std::make_unique<Page>());
    }

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
    static constexpr unsigned kPageBits = 13;
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

    void reserve(std::uint64_t ref_space, std::uint64_t max_live, std::size_t spare_hint) {
        pages_.reserve((ref_space >> kPageBits) + 1);
        pool_.reserve(max_live);
        free_.reserve(max_live);
        spare_.reserve(spare_hint);
        while (spare_.size() < spare_hint) spare_.push_back(std::make_unique<Page>());
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

// Open-addressing hash table over the ref space: fibonacci hashing, linear probing,
// backward-shift deletion (no tombstones, so probe chains never rot over a full day).
class FlatHashOrderStore {
    struct Slot {
        std::uint64_t ref;
        Order o;
    };

  public:
    FlatHashOrderStore() { rehash(std::size_t{1} << 22); }

    Order* find(std::uint64_t ref) {
        Slot& s = slots_[probe(ref)];
        return s.ref == ref && s.o.level != kNil ? &s.o : nullptr;
    }

    const Order* find(std::uint64_t ref) const {
        const Slot& s = slots_[probe(ref)];
        return s.ref == ref && s.o.level != kNil ? &s.o : nullptr;
    }

    Order& touch(std::uint64_t ref) {
        if ((size_ + 1) * 10 >= slots_.size() * 7) rehash(slots_.size() * 2);
        Slot& s = slots_[probe(ref)];
        if (s.ref != ref) {
            s.ref = ref;
            s.o.level = kNil;
            ++size_;
        }
        return s.o;
    }

    void note_live(std::uint64_t) {}

    void note_dead(std::uint64_t ref) { erase(probe(ref)); }

    void reserve(std::uint64_t, std::uint64_t max_live, std::size_t) {
        std::size_t want = slots_.size();
        while (max_live * 10 >= want * 7) want *= 2;
        if (want > slots_.size()) rehash(want);
    }

    std::uint64_t live_orders() const { return size_; }

  private:
    std::size_t home(std::uint64_t ref) const {
        return static_cast<std::size_t>((ref * 0x9e3779b97f4a7c15ull) >> shift_);
    }

    std::size_t probe(std::uint64_t ref) const {
        std::size_t i = home(ref);
        while (slots_[i].ref != ref && slots_[i].ref != kNilRef) i = (i + 1) & mask_;
        return i;
    }

    void erase(std::size_t i) {
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (slots_[j].ref == kNilRef) break;
            const std::size_t k = home(slots_[j].ref);
            if (((j - k) & mask_) >= ((j - i) & mask_)) {
                slots_[i] = slots_[j];
                i = j;
            }
        }
        slots_[i].ref = kNilRef;
        slots_[i].o.level = kNil;
        --size_;
    }

    void rehash(std::size_t new_cap) {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(new_cap, Slot{});
        mask_ = new_cap - 1;
        shift_ = 64 - static_cast<unsigned>(std::countr_zero(new_cap));
        for (const Slot& s : old) {
            if (s.ref == kNilRef) continue;
            std::size_t i = home(s.ref);
            while (slots_[i].ref != kNilRef) i = (i + 1) & mask_;
            slots_[i] = s;
        }
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    unsigned shift_ = 64;
    std::uint64_t size_ = 0;
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
