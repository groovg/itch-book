#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "messages.hpp"
#include "order_store.hpp"

namespace itch {

// Ask prices are stored negated so both sides sort ascending with the best level at the back.
struct Level {
    std::int64_t key;
    std::uint64_t shares;
    std::uint64_t head;
    std::uint64_t tail;
    std::uint32_t orders;
};

struct Top {
    Price price;
    std::uint64_t shares;
    std::uint32_t orders;
};

class Book {
  public:
    struct Entry {
        std::int64_t key;
        Handle level;
    };

    void add(OrderStore& os, std::uint64_t ref, Order& o, std::int64_t price_raw) {
        const bool buy = o.buy != 0;
        const std::int64_t key = buy ? price_raw : -price_raw;
        const Handle h = find_or_create(buy, key);
        Level& lv = levels_[h];
        lv.shares += o.qty;
        ++lv.orders;
        o.level = h;
        o.prev = lv.tail;
        o.next = kNilRef;
        if (lv.tail != kNilRef)
            os.touch(lv.tail).next = ref;
        else
            lv.head = ref;
        lv.tail = ref;
        os.note_live(ref);
    }

    std::uint32_t reduce(OrderStore& os, std::uint64_t ref, Order& o, std::uint32_t shares) {
        const std::uint32_t take = shares < o.qty ? shares : o.qty;
        if (take == o.qty) {
            remove(os, ref, o);
            return take;
        }
        levels_[o.level].shares -= take;
        o.qty -= take;
        return take;
    }

    void remove(OrderStore& os, std::uint64_t ref, Order& o) {
        const Handle h = o.level;
        Level& lv = levels_[h];
        lv.shares -= o.qty;
        if (o.prev != kNilRef)
            os.touch(o.prev).next = o.next;
        else
            lv.head = o.next;
        if (o.next != kNilRef)
            os.touch(o.next).prev = o.prev;
        else
            lv.tail = o.prev;
        if (--lv.orders == 0) erase_level(h);
        o.level = kNil;
        os.note_dead(ref);
    }

    Order* replace(OrderStore& os, std::uint64_t old_ref, Order& old_o, std::uint64_t new_ref,
                   std::uint32_t shares, std::int64_t price_raw) {
        const std::uint8_t buy = old_o.buy;
        remove(os, old_ref, old_o);
        Order& o = os.touch(new_ref);
        if (o.level != kNil) return nullptr;
        o.qty = shares;
        o.buy = buy;
        add(os, new_ref, o, price_raw);
        return &o;
    }

    bool top(bool buy, Top& out) const {
        const auto& s = buy ? bids_ : asks_;
        if (s.empty()) return false;
        const Level& lv = levels_[s.back().level];
        out = {Price::from_raw(buy ? lv.key : -lv.key), lv.shares, lv.orders};
        return true;
    }

    bool crossed() const {
        if (bids_.empty() || asks_.empty()) return false;
        return bids_.back().key >= -asks_.back().key;
    }

    const std::vector<Entry>& side(bool buy) const { return buy ? bids_ : asks_; }
    const Level& level(Handle h) const { return levels_[h]; }

    bool validate(const OrderStore& os) const {
        for (const bool buy : {true, false}) {
            const auto& s = buy ? bids_ : asks_;
            std::int64_t prev_key = INT64_MIN;
            for (const Entry& e : s) {
                if (e.key <= prev_key) return false;
                if (buy != (e.key > 0)) return false;
                prev_key = e.key;
                const Level& lv = levels_[e.level];
                if (lv.key != e.key) return false;
                std::uint64_t sum = 0;
                std::uint32_t count = 0;
                std::uint64_t prev_ref = kNilRef;
                for (std::uint64_t r = lv.head; r != kNilRef;) {
                    const Order* o = os.find(r);
                    if (!o || o->level != e.level || o->prev != prev_ref) return false;
                    sum += o->qty;
                    ++count;
                    prev_ref = r;
                    r = o->next;
                }
                if (lv.tail != prev_ref) return false;
                if (sum != lv.shares || count != lv.orders || count == 0) return false;
            }
        }
        return !crossed();
    }

  private:
    Handle find_or_create(bool buy, std::int64_t key) {
        std::vector<Entry>& s = buy ? bids_ : asks_;
        std::size_t i = s.size();
        while (i > 0 && s[i - 1].key > key) --i;
        if (i > 0 && s[i - 1].key == key) return s[i - 1].level;
        Handle h;
        if (!free_.empty()) {
            h = free_.back();
            free_.pop_back();
        } else {
            h = static_cast<Handle>(levels_.size());
            levels_.emplace_back();
        }
        levels_[h] = Level{key, 0, kNilRef, kNilRef, 0};
        s.insert(s.begin() + static_cast<std::ptrdiff_t>(i), Entry{key, h});
        return h;
    }

    void erase_level(Handle h) {
        const std::int64_t key = levels_[h].key;
        std::vector<Entry>& s = key > 0 ? bids_ : asks_;
        std::size_t i = s.size();
        while (s[i - 1].level != h) --i;
        s.erase(s.begin() + static_cast<std::ptrdiff_t>(i - 1));
        free_.push_back(h);
    }

    std::vector<Level> levels_;
    std::vector<Handle> free_;
    std::vector<Entry> bids_;
    std::vector<Entry> asks_;
};

}  // namespace itch
