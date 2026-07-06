#include <itch/book.hpp>
#include <itch/order_store.hpp>

#include "check.hpp"

#include <cstdint>
#include <vector>

using namespace itch;

namespace {

Order& add(OrderStore& os, Book<>& b, std::uint64_t ref, bool buy, std::uint32_t qty,
           std::int64_t px) {
    Order& o = os.touch(ref);
    o.qty = qty;
    o.buy = buy ? 1 : 0;
    b.add(os, ref, o, px);
    return o;
}

std::vector<std::uint64_t> chain(const OrderStore& os, const Book<>& b, bool buy) {
    std::vector<std::uint64_t> refs;
    const auto& s = b.side(buy);
    const Level& lv = b.level(s.back().level);
    for (std::uint64_t r = lv.head; r != kNilRef; r = os.find(r)->next) refs.push_back(r);
    return refs;
}

void empty_book() {
    OrderStore os;
    Book<> b;
    Top t{};
    CHECK(!b.top(true, t));
    CHECK(!b.top(false, t));
    CHECK(!b.crossed());
    CHECK(b.validate(os));
}

void price_ordering() {
    OrderStore os;
    Book<> b;
    add(os, b, 1, true, 100, 1'000'000);
    add(os, b, 2, true, 200, 1'001'000);
    add(os, b, 3, true, 300, 999'000);
    add(os, b, 4, false, 150, 1'003'000);
    add(os, b, 5, false, 250, 1'002'000);
    add(os, b, 6, false, 350, 1'004'000);

    Top bid{}, ask{};
    CHECK(b.top(true, bid) && bid.price.raw() == 1'001'000);
    CHECK(bid.shares == 200 && bid.orders == 1);
    CHECK(b.top(false, ask) && ask.price.raw() == 1'002'000);
    CHECK(ask.shares == 250 && ask.orders == 1);
    CHECK(b.side(true).size() == 3);
    CHECK(b.side(false).size() == 3);
    CHECK(!b.crossed());
    CHECK(b.validate(os));
}

void fifo_aggregation() {
    OrderStore os;
    Book<> b;
    add(os, b, 11, true, 100, 500'000);
    add(os, b, 12, true, 50, 500'000);
    add(os, b, 13, true, 25, 500'000);

    Top t{};
    CHECK(b.top(true, t) && t.shares == 175 && t.orders == 3);
    CHECK(b.side(true).size() == 1);
    auto refs = chain(os, b, true);
    CHECK(refs == (std::vector<std::uint64_t>{11, 12, 13}));
    CHECK(b.validate(os));
}

void partial_reduce() {
    OrderStore os;
    Book<> b;
    Order& o = add(os, b, 21, false, 100, 750'000);
    CHECK(b.reduce(os, 21, o, 40) == 40);
    Top t{};
    CHECK(b.top(false, t) && t.shares == 60 && t.orders == 1);
    CHECK(os.find(21) != nullptr);
    CHECK(b.validate(os));

    CHECK(b.reduce(os, 21, o, 60) == 60);
    CHECK(os.find(21) == nullptr);
    CHECK(!b.top(false, t));
    CHECK(b.validate(os));
}

void reduce_clamps() {
    OrderStore os;
    Book<> b;
    Order& o = add(os, b, 31, true, 30, 600'000);
    CHECK(b.reduce(os, 31, o, 100) == 30);
    CHECK(os.find(31) == nullptr);
    Top t{};
    CHECK(!b.top(true, t));
    CHECK(b.validate(os));
}

void unlink_middle() {
    OrderStore os;
    Book<> b;
    add(os, b, 41, true, 10, 800'000);
    Order& mid = add(os, b, 42, true, 20, 800'000);
    add(os, b, 43, true, 30, 800'000);

    b.remove(os, 42, mid);
    auto refs = chain(os, b, true);
    CHECK(refs == (std::vector<std::uint64_t>{41, 43}));
    Top t{};
    CHECK(b.top(true, t) && t.shares == 40 && t.orders == 2);
    CHECK(b.validate(os));

    Order& head = *os.find(41);
    b.remove(os, 41, head);
    refs = chain(os, b, true);
    CHECK(refs == (std::vector<std::uint64_t>{43}));
    CHECK(b.validate(os));
}

void level_erase_away_from_touch() {
    OrderStore os;
    Book<> b;
    add(os, b, 51, false, 10, 900'000);
    Order& deep = add(os, b, 52, false, 20, 990'000);
    add(os, b, 53, false, 30, 950'000);
    CHECK(b.side(false).size() == 3);

    b.remove(os, 52, deep);
    CHECK(b.side(false).size() == 2);
    Top t{};
    CHECK(b.top(false, t) && t.price.raw() == 900'000);
    CHECK(b.validate(os));
}

void page_reclamation() {
    OrderStore os;
    Book<> b;
    Order& a = add(os, b, 61, true, 10, 400'000);
    Order& c = add(os, b, 62, false, 20, 410'000);
    CHECK(os.resident_pages() == 1);

    b.remove(os, 61, a);
    CHECK(os.resident_pages() == 1);
    b.remove(os, 62, c);
    CHECK(os.resident_pages() == 0);
    CHECK(os.spare_pages() == 1);

    add(os, b, 63, true, 5, 420'000);
    CHECK(os.resident_pages() == 1);
    CHECK(os.spare_pages() == 0);
    CHECK(os.find(61) == nullptr);
    CHECK(os.find(62) == nullptr);
    CHECK(os.find(63) != nullptr);
    CHECK(b.validate(os));
}

void replace_moves_level() {
    OrderStore os;
    Book<> b;
    Order& o = add(os, b, 81, true, 100, 1'000'000);
    add(os, b, 82, true, 40, 1'000'000);

    Order* n = b.replace(os, 81, o, 83, 60, 1'002'000);
    CHECK(n != nullptr);
    CHECK(os.find(81) == nullptr);
    CHECK(os.find(83) != nullptr);
    CHECK(n->buy == 1);
    Top t{};
    CHECK(b.top(true, t) && t.price.raw() == 1'002'000 && t.shares == 60);
    CHECK(b.side(true).size() == 2);
    CHECK(b.validate(os));
}

void replace_same_price_requeues() {
    OrderStore os;
    Book<> b;
    Order& first = add(os, b, 91, false, 10, 2'000'000);
    add(os, b, 92, false, 20, 2'000'000);

    CHECK(b.replace(os, 91, first, 99, 15, 2'000'000) != nullptr);
    auto refs = chain(os, b, false);
    CHECK(refs == (std::vector<std::uint64_t>{92, 99}));
    Top t{};
    CHECK(b.top(false, t) && t.shares == 35 && t.orders == 2);
    CHECK(b.validate(os));
}

void replace_after_partial_execute() {
    OrderStore os;
    Book<> b;
    Order& o = add(os, b, 101, true, 100, 3'000'000);
    CHECK(b.reduce(os, 101, o, 70) == 70);
    CHECK(os.find(101) != nullptr);

    CHECK(b.replace(os, 101, o, 102, 500, 3'000'000) != nullptr);
    Top t{};
    CHECK(b.top(true, t) && t.shares == 500 && t.orders == 1);
    CHECK(b.validate(os));
}

void chained_replace() {
    OrderStore os;
    Book<> b;
    Order& a = add(os, b, 111, false, 10, 4'000'000);
    Order* second = b.replace(os, 111, a, 112, 20, 4'100'000);
    CHECK(second != nullptr);
    Order* third = b.replace(os, 112, *second, 113, 30, 4'200'000);
    CHECK(third != nullptr);

    CHECK(os.find(111) == nullptr);
    CHECK(os.find(112) == nullptr);
    CHECK(os.find(113) != nullptr);
    CHECK(third->buy == 0);
    Top t{};
    CHECK(b.top(false, t) && t.price.raw() == 4'200'000 && t.shares == 30);
    CHECK(b.side(false).size() == 1);
    CHECK(b.validate(os));
}

void replace_duplicate_new_ref() {
    OrderStore os;
    Book<> b;
    Order& a = add(os, b, 121, true, 10, 5'000'000);
    add(os, b, 122, true, 20, 5'100'000);

    CHECK(b.replace(os, 121, a, 122, 30, 5'200'000) == nullptr);
    CHECK(os.find(121) == nullptr);
    Top t{};
    CHECK(b.top(true, t) && t.price.raw() == 5'100'000 && t.shares == 20);
    CHECK(b.validate(os));
}

void crossed_detection() {
    OrderStore os;
    Book<> b;
    add(os, b, 71, true, 10, 1'010'000);
    add(os, b, 72, false, 10, 1'000'000);
    CHECK(b.crossed());
    CHECK(b.validate(os));
}

}  // namespace

int main() {
    empty_book();
    price_ordering();
    fifo_aggregation();
    partial_reduce();
    reduce_clamps();
    unlink_middle();
    level_erase_away_from_touch();
    page_reclamation();
    replace_moves_level();
    replace_same_price_requeues();
    replace_after_partial_execute();
    chained_replace();
    replace_duplicate_new_ref();
    crossed_detection();
    RUN_END();
}
