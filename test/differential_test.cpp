#include <itch/book_manager.hpp>
#include <itch/parser.hpp>

#include "check.hpp"
#include "reference.hpp"
#include "synthetic.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

template <typename BookT>
bool side_matches(const std::map<std::int64_t, refimpl::RefLevel>& ref_side, const BookT& b,
                  bool buy) {
    const auto& s = b.side(buy);
    if (s.size() != ref_side.size()) return false;
    std::size_t i = 0;
    if (buy) {
        for (const auto& [price, lv] : ref_side) {
            const auto& e = s[i++];
            const itch::Level& fast = b.level(e.level);
            if (e.key != price || fast.shares != lv.shares || fast.orders != lv.orders)
                return false;
        }
    } else {
        for (auto it = ref_side.rbegin(); it != ref_side.rend(); ++it) {
            const auto& e = s[i++];
            const itch::Level& fast = b.level(e.level);
            if (-e.key != it->first || fast.shares != it->second.shares ||
                fast.orders != it->second.orders)
                return false;
        }
    }
    return true;
}

template <typename Mgr>
bool books_match(const refimpl::Reference& ref, const Mgr& mgr) {
    if (mgr.orders().live_orders() != ref.live()) {
        std::fprintf(stderr, "live mismatch: fast %llu ref %zu\n",
                     static_cast<unsigned long long>(mgr.orders().live_orders()), ref.live());
        return false;
    }
    static const refimpl::RefBook empty{};
    std::size_t max_locate = mgr.book_count();
    if (!ref.books().empty())
        if (const auto last = ref.books().rbegin()->first + 1u; last > max_locate)
            max_locate = last;
    for (std::size_t loc = 0; loc < max_locate; ++loc) {
        const auto it = ref.books().find(static_cast<std::uint16_t>(loc));
        const refimpl::RefBook& rb = it == ref.books().end() ? empty : it->second;
        const auto* fb = mgr.book(static_cast<std::uint16_t>(loc));
        if (!fb) {
            if (!rb.bids.empty() || !rb.asks.empty()) {
                std::fprintf(stderr, "locate %zu: missing fast book\n", loc);
                return false;
            }
            continue;
        }
        if (!fb->validate(mgr.orders())) {
            std::fprintf(stderr, "locate %zu: structural validation failed\n", loc);
            return false;
        }
        if (!side_matches(rb.bids, *fb, true) || !side_matches(rb.asks, *fb, false)) {
            std::fprintf(stderr, "locate %zu: side mismatch\n", loc);
            return false;
        }
    }
    return true;
}

template <typename Mgr>
void run_seed(std::uint64_t seed) {
    synth::Generator g(seed, 25);
    refimpl::Reference ref;
    Mgr mgr;
    std::vector<std::byte> chunk;

    g.preamble(chunk);
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 80'000; ++i) g.step(chunk);
        const auto r1 = itch::parse(chunk, mgr);
        const auto r2 = itch::parse(chunk, ref);
        CHECK(r1.messages == r2.messages);
        CHECK(r1.malformed == 0);
        CHECK(books_match(ref, mgr));
        chunk.clear();
    }
    g.finish(chunk);
    const auto r = itch::parse(chunk, mgr);
    itch::parse(chunk, ref);
    CHECK(r.end_of_session);
    CHECK(books_match(ref, mgr));
}

}  // namespace

int main() {
    using Inline = itch::BookManager<std::nullptr_t, itch::Book<itch::OrderStore>>;
    using Pooled = itch::BookManager<std::nullptr_t, itch::Book<itch::PooledOrderStore>>;
    using Hashed = itch::BookManager<std::nullptr_t, itch::Book<itch::HashOrderStore>>;
    using FlatHashed = itch::BookManager<std::nullptr_t, itch::Book<itch::FlatHashOrderStore>>;
    for (const std::uint64_t seed : {1, 42, 20260705}) {
        run_seed<Inline>(seed);
        run_seed<Pooled>(seed);
        run_seed<Hashed>(seed);
        run_seed<FlatHashed>(seed);
    }
    RUN_END();
}
