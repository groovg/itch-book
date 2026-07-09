#pragma once

#include <cstdint>
#include <type_traits>
#include <vector>

#include "book.hpp"
#include "messages.hpp"
#include "order_store.hpp"

namespace itch {

struct Bbo {
    Top bid{};
    Top ask{};
    bool has_bid = false;
    bool has_ask = false;

    bool same(const Bbo& other) const {
        if (has_bid != other.has_bid || has_ask != other.has_ask) return false;
        if (has_bid && (bid.price.raw() != other.bid.price.raw() ||
                        bid.shares != other.bid.shares))
            return false;
        if (has_ask && (ask.price.raw() != other.ask.price.raw() ||
                        ask.shares != other.ask.shares))
            return false;
        return true;
    }
};

struct Stats {
    std::uint64_t adds = 0;
    std::uint64_t executes = 0;
    std::uint64_t cancels = 0;
    std::uint64_t deletes = 0;
    std::uint64_t replaces = 0;
    std::uint64_t missing_ref = 0;
    std::uint64_t dup_ref = 0;
    std::uint64_t rejected = 0;
    std::uint64_t clamped = 0;
};

// One time-and-sales record. source: 'E' execute at the resting price, 'C' execute at a
// message price (printable only), 'P' non-cross trade, 'Q' cross, 'B' broken (voids a
// prior match; price/shares are zero). side is the resting order's side where known.
struct TradePrint {
    std::uint64_t timestamp;
    std::uint64_t match;
    Price price;
    std::uint64_t shares;
    std::uint16_t locate;
    char source;
    char side;
};

template <typename OnBbo = std::nullptr_t, typename BookT = Book<>,
          typename OnTrade = std::nullptr_t>
class BookManager {
    static constexpr bool kBbo = !std::is_same_v<OnBbo, std::nullptr_t>;
    static constexpr bool kTrades = !std::is_same_v<OnTrade, std::nullptr_t>;

  public:
    using Store = typename BookT::StoreType;

    BookManager() = default;
    explicit BookManager(OnBbo on_bbo) : on_bbo_(std::move(on_bbo)) {}
    BookManager(OnBbo on_bbo, OnTrade on_trade)
        : on_bbo_(std::move(on_bbo)), on_trade_(std::move(on_trade)) {}

    void on_stock_directory(const StockDirectory& m) {
        ensure(m.hdr.locate);
        symbols_[m.hdr.locate] = m.stock;
    }

    void on_trading_action(const TradingAction& m) {
        ensure(m.hdr.locate);
        state_[m.hdr.locate] = m.state;
    }

    void on_add(const AddOrder& m) {
        if (m.shares == 0 || m.price.raw() <= 0 || m.ref == kNilRef || m.ref >= kMaxRef) {
            ++stats_.rejected;
            return;
        }
        BookT& b = ensure(m.hdr.locate);
        Order* o = &os_.touch(m.ref);
        if (o->level != kNil) {
            ++stats_.dup_ref;
            books_[o->locate].remove(os_, m.ref, *o);
            o = &os_.touch(m.ref);
        }
        o->qty = m.shares;
        o->locate = m.hdr.locate;
        o->buy = m.side == Side::Buy ? 1 : 0;
        b.add(os_, m.ref, *o, m.price.raw());
        ++stats_.adds;
        check_top(m.hdr.locate);
    }

    void on_execute(const OrderExecuted& m) {
        if constexpr (kTrades) {
            if (const Order* o = os_.find(m.ref)) {
                const Level& lv = books_[o->locate].level(o->level);
                emit(o->locate, m.hdr.timestamp, Price::from_raw(o->buy ? lv.key : -lv.key),
                     m.shares, m.match, 'E', o->buy ? 'B' : 'S');
            }
        }
        reduce(m.ref, m.shares, stats_.executes);
    }

    void on_execute_price(const OrderExecutedPrice& m) {
        if constexpr (kTrades) {
            if (m.printable) {
                if (const Order* o = os_.find(m.ref))
                    emit(o->locate, m.hdr.timestamp, m.price, m.shares, m.match, 'C',
                         o->buy ? 'B' : 'S');
            }
        }
        reduce(m.ref, m.shares, stats_.executes);
    }

    void on_trade(const Trade& m)
        requires kTrades
    {
        emit(m.hdr.locate, m.hdr.timestamp, m.price, m.shares, m.match, 'P',
             static_cast<char>(m.side));
    }

    void on_cross(const CrossTrade& m)
        requires kTrades
    {
        emit(m.hdr.locate, m.hdr.timestamp, m.price, m.shares, m.match, 'Q', ' ');
    }

    void on_broken(const BrokenTrade& m)
        requires kTrades
    {
        emit(m.hdr.locate, m.hdr.timestamp, Price::from_raw(0), 0, m.match, 'B', ' ');
    }

    void on_cancel(const OrderCancel& m) { reduce(m.ref, m.shares, stats_.cancels); }

    void on_delete(const OrderDelete& m) {
        Order* o = os_.find(m.ref);
        if (!o) {
            ++stats_.missing_ref;
            return;
        }
        const std::uint16_t loc = o->locate;
        books_[loc].remove(os_, m.ref, *o);
        ++stats_.deletes;
        check_top(loc);
    }

    void on_replace(const OrderReplace& m) {
        Order* o = os_.find(m.old_ref);
        if (!o) {
            ++stats_.missing_ref;
            return;
        }
        const std::uint16_t loc = o->locate;
        BookT& b = books_[loc];
        if (m.shares == 0 || m.price.raw() <= 0 || m.new_ref == kNilRef ||
            m.new_ref >= kMaxRef) {
            b.remove(os_, m.old_ref, *o);
            ++stats_.rejected;
        } else {
            Order* n = b.replace(os_, m.old_ref, *o, m.new_ref, m.shares, m.price.raw());
            if (n)
                n->locate = loc;
            else
                ++stats_.dup_ref;
            ++stats_.replaces;
        }
        check_top(loc);
    }

    const BookT* book(std::uint16_t locate) const {
        return locate < books_.size() ? &books_[locate] : nullptr;
    }

    wire::Alpha<8> symbol(std::uint16_t locate) const {
        return locate < symbols_.size() ? symbols_[locate]
                                        : wire::Alpha<8>{{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '}};
    }

    // Latest Stock Trading Action state: 'T' trading, 'H' halted, 'P' paused,
    // 'Q' quotation only; ' ' until the first action arrives. Books are intentionally
    // not gated on this — order maintenance continues during halts.
    char trading_state(std::uint16_t locate) const {
        return locate < state_.size() ? state_[locate] : ' ';
    }

    Bbo bbo(std::uint16_t locate) const {
        Bbo r{};
        if (const BookT* b = book(locate)) {
            r.has_bid = b->top(true, r.bid);
            r.has_ask = b->top(false, r.ask);
        }
        return r;
    }

    void reserve(std::uint16_t max_locate, std::size_t levels_per_book) {
        ensure(max_locate);
        for (BookT& b : books_) b.reserve(levels_per_book);
    }

    std::size_t book_count() const { return books_.size(); }
    const Stats& stats() const { return stats_; }
    const Store& orders() const { return os_; }
    Store& orders() { return os_; }

  private:
    BookT& ensure(std::uint16_t locate) {
        if (locate >= books_.size()) {
            books_.resize(locate + 1);
            symbols_.resize(locate + 1, wire::Alpha<8>{{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '}});
            last_.resize(locate + 1);
            state_.resize(locate + 1, ' ');
        }
        return books_[locate];
    }

    void emit(std::uint16_t locate, std::uint64_t ts, Price price, std::uint64_t shares,
              std::uint64_t match, char source, char side) {
        if constexpr (kTrades)
            on_trade_(TradePrint{ts, match, price, shares, locate, source, side});
    }

    void reduce(std::uint64_t ref, std::uint32_t shares, std::uint64_t& counter) {
        Order* o = os_.find(ref);
        if (!o) {
            ++stats_.missing_ref;
            return;
        }
        const std::uint16_t loc = o->locate;
        if (books_[loc].reduce(os_, ref, *o, shares) != shares) ++stats_.clamped;
        ++counter;
        check_top(loc);
    }

    void check_top(std::uint16_t locate) {
        if constexpr (kBbo) {
            const BookT& b = books_[locate];
            Bbo now{};
            now.has_bid = b.top(true, now.bid);
            now.has_ask = b.top(false, now.ask);
            if (!now.same(last_[locate])) {
                last_[locate] = now;
                on_bbo_(locate, now);
            }
        }
    }

    Store os_;
    std::vector<BookT> books_;
    std::vector<wire::Alpha<8>> symbols_;
    std::vector<Bbo> last_;
    std::vector<char> state_;
    Stats stats_;
    [[no_unique_address]] OnBbo on_bbo_{};
    [[no_unique_address]] OnTrade on_trade_{};
};

}  // namespace itch
