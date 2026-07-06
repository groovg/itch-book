#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>

#include <itch/messages.hpp>
#include <itch/order_store.hpp>

namespace refimpl {

struct RefOrder {
    std::uint16_t locate;
    bool buy;
    std::int64_t price;
    std::uint64_t qty;
};

struct RefLevel {
    std::uint64_t shares = 0;
    std::uint32_t orders = 0;
};

struct RefBook {
    std::map<std::int64_t, RefLevel> bids;
    std::map<std::int64_t, RefLevel> asks;
};

class Reference {
  public:
    void on_add(const itch::AddOrder& m) {
        if (m.shares == 0 || m.price.raw() <= 0 || m.ref == 0 || m.ref >= itch::kMaxRef) return;
        auto it = orders_.find(m.ref);
        if (it != orders_.end()) remove_order(it);
        insert(m.ref, {m.hdr.locate, m.side == itch::Side::Buy, m.price.raw(), m.shares});
    }

    void on_execute(const itch::OrderExecuted& m) { reduce(m.ref, m.shares); }
    void on_execute_price(const itch::OrderExecutedPrice& m) { reduce(m.ref, m.shares); }
    void on_cancel(const itch::OrderCancel& m) { reduce(m.ref, m.shares); }

    void on_delete(const itch::OrderDelete& m) {
        auto it = orders_.find(m.ref);
        if (it != orders_.end()) remove_order(it);
    }

    void on_replace(const itch::OrderReplace& m) {
        auto it = orders_.find(m.old_ref);
        if (it == orders_.end()) return;
        const RefOrder old = it->second;
        remove_order(it);
        if (m.shares == 0 || m.price.raw() <= 0 || m.new_ref == 0 ||
            m.new_ref >= itch::kMaxRef)
            return;
        if (orders_.find(m.new_ref) != orders_.end()) return;
        insert(m.new_ref, {old.locate, old.buy, m.price.raw(), m.shares});
    }

    const std::map<std::uint16_t, RefBook>& books() const { return books_; }
    std::size_t live() const { return orders_.size(); }

  private:
    using OrderMap = std::unordered_map<std::uint64_t, RefOrder>;

    void insert(std::uint64_t ref, RefOrder o) {
        orders_.emplace(ref, o);
        RefLevel& lv = side_of(o)[o.price];
        lv.shares += o.qty;
        ++lv.orders;
    }

    void reduce(std::uint64_t ref, std::uint64_t shares) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) return;
        RefOrder& o = it->second;
        const std::uint64_t take = shares < o.qty ? shares : o.qty;
        if (take == o.qty) {
            remove_order(it);
            return;
        }
        o.qty -= take;
        side_of(o).find(o.price)->second.shares -= take;
    }

    void remove_order(OrderMap::iterator it) {
        const RefOrder o = it->second;
        auto& side = side_of(o);
        auto lv = side.find(o.price);
        lv->second.shares -= o.qty;
        if (--lv->second.orders == 0) side.erase(lv);
        orders_.erase(it);
    }

    std::map<std::int64_t, RefLevel>& side_of(const RefOrder& o) {
        RefBook& b = books_[o.locate];
        return o.buy ? b.bids : b.asks;
    }

    OrderMap orders_;
    std::map<std::uint16_t, RefBook> books_;
};

}  // namespace refimpl
