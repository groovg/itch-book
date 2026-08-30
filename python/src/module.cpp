#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <itch/book_manager.hpp>
#include <itch/stream.hpp>

namespace nb = nanobind;

namespace {

nb::str ch(char c) { return nb::str(&c, 1); }

struct BboColumns {
    std::vector<std::uint64_t> ts, seq;
    std::vector<std::uint16_t> locate;
    std::vector<std::int64_t> bid_px, ask_px;
    std::vector<std::uint32_t> bid_sz, bid_ct, ask_sz, ask_ct;

    template <typename F>
    void each(F f) {
        f(ts); f(seq); f(locate); f(bid_px); f(bid_sz); f(bid_ct); f(ask_px); f(ask_sz); f(ask_ct);
    }
};

struct TradeColumns {
    std::vector<std::uint64_t> ts, seq, size, order_id, match;
    std::vector<std::uint16_t> locate;
    std::vector<std::uint8_t> kind, side, cross_type;
    std::vector<std::int64_t> px;

    template <typename F>
    void each(F f) {
        f(ts); f(seq); f(size); f(order_id); f(match); f(locate); f(kind); f(side); f(cross_type); f(px);
    }
};

struct MessageColumns {
    std::vector<std::uint64_t> ts, seq, order_id, old_order_id;
    std::vector<std::uint16_t> locate, mpid;
    std::vector<std::uint8_t> type, action, side, printable;
    std::vector<std::int64_t> px;
    std::vector<std::uint32_t> size, remaining;

    template <typename F>
    void each(F f) {
        f(ts); f(seq); f(order_id); f(old_order_id); f(locate); f(mpid); f(type); f(action); f(side);
        f(printable); f(px); f(size); f(remaining);
    }
};

struct EventColumns {
    std::vector<std::uint64_t> ts, seq;
    std::vector<std::uint8_t> event;
};

class Session {
    struct Sink {
        Session* s;
        void operator()(std::uint16_t locate, const itch::Bbo& b) const { s->emit_bbo(locate, b); }
    };
    struct TradeSink {
        Session* s;
        void operator()(const itch::TradePrint& p) const { s->emit_trade(p); }
    };
    using Manager = itch::BookManager<Sink, itch::Book<>, TradeSink>;

    struct Resting {
        std::uint16_t locate;
        char side;
        std::int64_t px;
        std::uint32_t qty;
        bool found;
    };

    struct Handler {
        Session& s;
        Manager& mgr;

        void on_system_event(const itch::SystemEvent& m) {
            s.stamp(m.hdr);
            s.last_event_ = m.event;
            s.events_.ts.push_back(s.ts_);
            s.events_.seq.push_back(s.seq_);
            s.events_.event.push_back(static_cast<std::uint8_t>(m.event));
        }
        void on_stock_directory(const itch::StockDirectory& m) {
            s.stamp(m.hdr);
            mgr.on_stock_directory(m);
            s.symbols_.push_back(m);
        }
        void on_trading_action(const itch::TradingAction& m) {
            s.stamp(m.hdr);
            mgr.on_trading_action(m);
        }
        void on_add(const itch::AddOrder& m) {
            s.stamp(m.hdr);
            const std::uint64_t rejected = mgr.stats().rejected;
            mgr.on_add(m);
            if (s.messages_on_)
                s.emit_message(m.hdr.locate, m.attributed ? 'F' : 'A', 'A',
                               static_cast<char>(m.side), m.price.raw(), m.shares,
                               mgr.stats().rejected == rejected ? m.shares : 0, m.ref, 0,
                               m.attributed ? s.mpid_id(m.mpid) : 0, true);
        }
        void on_execute(const itch::OrderExecuted& m) {
            s.stamp(m.hdr);
            if (s.messages_on_) {
                const Resting r = s.resting(m.ref, m.hdr.locate);
                s.emit_message(r.locate, 'E', 'F', r.side, r.px, m.shares, s.left(r, m.shares),
                               m.ref, 0, 0, true);
            }
            mgr.on_execute(m);
        }
        void on_execute_price(const itch::OrderExecutedPrice& m) {
            s.stamp(m.hdr);
            if (s.messages_on_) {
                const Resting r = s.resting(m.ref, m.hdr.locate);
                s.emit_message(r.locate, 'C', 'F', r.side, m.price.raw(), m.shares,
                               s.left(r, m.shares), m.ref, 0, 0, m.printable);
            }
            mgr.on_execute_price(m);
        }
        void on_cancel(const itch::OrderCancel& m) {
            s.stamp(m.hdr);
            if (s.messages_on_) {
                const Resting r = s.resting(m.ref, m.hdr.locate);
                s.emit_message(r.locate, 'X', 'C', r.side, r.px, m.shares, s.left(r, m.shares),
                               m.ref, 0, 0, true);
            }
            mgr.on_cancel(m);
        }
        void on_delete(const itch::OrderDelete& m) {
            s.stamp(m.hdr);
            if (s.messages_on_) {
                const Resting r = s.resting(m.ref, m.hdr.locate);
                s.emit_message(r.locate, 'D', 'C', r.side, r.px, r.qty, 0, m.ref, 0, 0, true);
            }
            mgr.on_delete(m);
        }
        void on_replace(const itch::OrderReplace& m) {
            s.stamp(m.hdr);
            const Resting r = s.messages_on_ ? s.resting(m.old_ref, m.hdr.locate) : Resting{};
            const std::uint64_t rejected = mgr.stats().rejected;
            mgr.on_replace(m);
            if (s.messages_on_)
                s.emit_message(r.locate, 'U', 'M', r.side, m.price.raw(), m.shares,
                               r.found && mgr.stats().rejected == rejected ? m.shares : 0,
                               m.new_ref, m.old_ref, 0, true);
        }
        void on_trade(const itch::Trade& m) {
            s.stamp(m.hdr);
            mgr.on_trade(m);
        }
        void on_cross(const itch::CrossTrade& m) {
            s.stamp(m.hdr);
            mgr.on_cross(m);
        }
        void on_broken(const itch::BrokenTrade& m) {
            s.stamp(m.hdr);
            mgr.on_broken(m);
        }
        void on_other(char) { ++s.seq_; }
    };

  public:
    Session(bool bbo, bool trades, bool messages)
        : mgr_(Sink{this}, TradeSink{this}),
          handler_{*this, mgr_},
          parser_(handler_),
          bbo_on_(bbo),
          trades_on_(trades),
          messages_on_(messages) {
        mpids_.emplace_back();
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void reserve(std::size_t rows) {
        reserve_ = rows;
        auto grow = [rows](auto& v) { v.reserve(rows); };
        if (bbo_on_) bbo_.each(grow);
        if (trades_on_) trades_.each(grow);
        if (messages_on_) messages_.each(grow);
    }

    void feed(const std::uint8_t* p, std::size_t n) {
        parser_.feed({reinterpret_cast<const std::byte*>(p), n});
    }

    std::size_t rows() const {
        std::size_t n = bbo_.ts.size();
        if (trades_.ts.size() > n) n = trades_.ts.size();
        if (messages_.ts.size() > n) n = messages_.ts.size();
        return n;
    }

    nb::dict take_bbo() {
        nb::dict d;
        d["ts"] = take(bbo_.ts);
        d["seq"] = take(bbo_.seq);
        d["locate"] = take(bbo_.locate);
        d["bid_px"] = take(bbo_.bid_px);
        d["bid_sz"] = take(bbo_.bid_sz);
        d["bid_ct"] = take(bbo_.bid_ct);
        d["ask_px"] = take(bbo_.ask_px);
        d["ask_sz"] = take(bbo_.ask_sz);
        d["ask_ct"] = take(bbo_.ask_ct);
        return d;
    }

    nb::dict take_trades() {
        nb::dict d;
        d["ts"] = take(trades_.ts);
        d["seq"] = take(trades_.seq);
        d["locate"] = take(trades_.locate);
        d["kind"] = take(trades_.kind);
        d["px"] = take(trades_.px);
        d["size"] = take(trades_.size);
        d["side"] = take(trades_.side);
        d["order_id"] = take(trades_.order_id);
        d["match"] = take(trades_.match);
        d["cross_type"] = take(trades_.cross_type);
        return d;
    }

    nb::dict take_messages() {
        nb::dict d;
        d["ts"] = take(messages_.ts);
        d["seq"] = take(messages_.seq);
        d["locate"] = take(messages_.locate);
        d["type"] = take(messages_.type);
        d["action"] = take(messages_.action);
        d["side"] = take(messages_.side);
        d["px"] = take(messages_.px);
        d["size"] = take(messages_.size);
        d["remaining"] = take(messages_.remaining);
        d["printable"] = take(messages_.printable);
        d["order_id"] = take(messages_.order_id);
        d["old_order_id"] = take(messages_.old_order_id);
        d["mpid"] = take(messages_.mpid);
        return d;
    }

    nb::dict take_events() {
        nb::dict d;
        d["ts"] = take(events_.ts);
        d["seq"] = take(events_.seq);
        d["event"] = take(events_.event);
        return d;
    }

    nb::list mpids() const {
        nb::list out;
        for (const std::string& m : mpids_) out.append(nb::str(m.c_str(), m.size()));
        return out;
    }

    nb::list take_symbols() {
        nb::list rows;
        for (const itch::StockDirectory& m : symbols_) {
            rows.append(nb::make_tuple(
                m.hdr.locate, nb::str(m.stock.view().data(), m.stock.view().size()),
                ch(m.market_category), ch(m.financial_status), m.round_lot_size,
                m.round_lots_only, ch(m.issue_classification),
                nb::str(m.issue_subtype.view().data(), m.issue_subtype.view().size()),
                ch(m.authenticity), ch(m.short_sale_threshold), ch(m.ipo_flag), ch(m.luld_tier),
                ch(m.etp_flag), m.etp_leverage, ch(m.inverse)));
        }
        symbols_.clear();
        return rows;
    }

    nb::dict stats() const {
        const itch::ParseResult& r = parser_.result();
        const itch::Stats& s = mgr_.stats();
        std::uint64_t crossed = 0;
        for (std::size_t i = 0; i < mgr_.book_count(); ++i)
            if (mgr_.book(static_cast<std::uint16_t>(i))->crossed()) ++crossed;
        nb::dict d;
        d["messages"] = r.messages;
        d["unknown"] = r.unknown;
        d["malformed"] = r.malformed;
        d["end_of_session"] = r.end_of_session;
        d["last_event"] = ch(last_event_);
        d["pending_bytes"] = parser_.pending_bytes();
        d["adds"] = s.adds;
        d["executes"] = s.executes;
        d["cancels"] = s.cancels;
        d["deletes"] = s.deletes;
        d["replaces"] = s.replaces;
        d["missing_ref"] = s.missing_ref;
        d["dup_ref"] = s.dup_ref;
        d["rejected"] = s.rejected;
        d["clamped"] = s.clamped;
        d["books"] = mgr_.book_count();
        d["live_orders"] = mgr_.orders().live_orders();
        d["crossed_books"] = crossed;
        return d;
    }

  private:
    template <typename T>
    nb::ndarray<nb::numpy, T, nb::ndim<1>> take(std::vector<T>& v) {
        auto* heap = new std::vector<T>(std::move(v));
        nb::capsule owner(heap, [](void* p) noexcept { delete static_cast<std::vector<T>*>(p); });
        v.reserve(reserve_);
        return nb::ndarray<nb::numpy, T, nb::ndim<1>>(heap->data(), {heap->size()}, owner);
    }

    void stamp(const itch::Header& h) {
        ts_ = h.timestamp;
        ++seq_;
    }

    Resting resting(std::uint64_t ref, std::uint16_t fallback_locate) const {
        if (const itch::Order* o = mgr_.orders().find(ref)) {
            const itch::Level& lv = mgr_.book(o->locate)->level(o->level);
            return {o->locate, o->buy ? 'B' : 'S', o->buy ? lv.key : -lv.key, o->qty, true};
        }
        return {fallback_locate, 'N', 0, 0, false};
    }

    static std::uint32_t left(const Resting& r, std::uint32_t taken) {
        return r.qty > taken ? r.qty - taken : 0;
    }

    std::uint16_t mpid_id(const itch::wire::Alpha<4>& mpid) {
        std::uint32_t key;
        std::memcpy(&key, mpid.raw.data(), 4);
        auto it = mpid_ids_.find(key);
        if (it != mpid_ids_.end()) return it->second;
        const auto id = static_cast<std::uint16_t>(mpids_.size());
        mpids_.emplace_back(mpid.view());
        mpid_ids_.emplace(key, id);
        return id;
    }

    void emit_bbo(std::uint16_t locate, const itch::Bbo& b) {
        if (!bbo_on_) return;
        bbo_.ts.push_back(ts_);
        bbo_.seq.push_back(seq_);
        bbo_.locate.push_back(locate);
        bbo_.bid_px.push_back(b.has_bid ? b.bid.price.raw() : 0);
        bbo_.bid_sz.push_back(b.has_bid ? static_cast<std::uint32_t>(b.bid.shares) : 0);
        bbo_.bid_ct.push_back(b.has_bid ? b.bid.orders : 0);
        bbo_.ask_px.push_back(b.has_ask ? b.ask.price.raw() : 0);
        bbo_.ask_sz.push_back(b.has_ask ? static_cast<std::uint32_t>(b.ask.shares) : 0);
        bbo_.ask_ct.push_back(b.has_ask ? b.ask.orders : 0);
    }

    void emit_trade(const itch::TradePrint& p) {
        if (!trades_on_) return;
        trades_.ts.push_back(p.timestamp);
        trades_.seq.push_back(seq_);
        trades_.locate.push_back(p.locate);
        trades_.kind.push_back(static_cast<std::uint8_t>(p.source));
        trades_.px.push_back(p.price.raw());
        trades_.size.push_back(p.shares);
        trades_.side.push_back(static_cast<std::uint8_t>(p.side == ' ' ? 'N' : p.side));
        trades_.order_id.push_back(p.order_id);
        trades_.match.push_back(p.match);
        trades_.cross_type.push_back(static_cast<std::uint8_t>(p.cross_type == ' ' ? 'N' : p.cross_type));
    }

    void emit_message(std::uint16_t locate, char type, char action, char side, std::int64_t px,
                      std::uint32_t size, std::uint32_t remaining, std::uint64_t order_id,
                      std::uint64_t old_order_id, std::uint16_t mpid, bool printable) {
        messages_.ts.push_back(ts_);
        messages_.seq.push_back(seq_);
        messages_.locate.push_back(locate);
        messages_.type.push_back(static_cast<std::uint8_t>(type));
        messages_.action.push_back(static_cast<std::uint8_t>(action));
        messages_.side.push_back(static_cast<std::uint8_t>(side));
        messages_.px.push_back(px);
        messages_.size.push_back(size);
        messages_.remaining.push_back(remaining);
        messages_.printable.push_back(printable ? 1 : 0);
        messages_.order_id.push_back(order_id);
        messages_.old_order_id.push_back(old_order_id);
        messages_.mpid.push_back(mpid);
    }

    Manager mgr_;
    Handler handler_;
    itch::StreamParser<Handler> parser_;
    BboColumns bbo_;
    TradeColumns trades_;
    MessageColumns messages_;
    EventColumns events_;
    std::vector<itch::StockDirectory> symbols_;
    std::vector<std::string> mpids_;
    std::unordered_map<std::uint32_t, std::uint16_t> mpid_ids_;
    std::size_t reserve_ = 0;
    std::uint64_t ts_ = 0;
    std::uint64_t seq_ = 0;
    char last_event_ = ' ';
    bool bbo_on_, trades_on_, messages_on_;
};

}  // namespace

NB_MODULE(_core, m) {
    nb::class_<Session>(m, "Session")
        .def(nb::init<bool, bool, bool>(), nb::arg("bbo"), nb::arg("trades"), nb::arg("messages"))
        .def("reserve", &Session::reserve)
        .def(
            "feed",
            [](Session& s, nb::ndarray<const std::uint8_t, nb::ndim<1>, nb::c_contig, nb::device::cpu> buf) {
                const std::uint8_t* p = buf.data();
                const std::size_t n = buf.shape(0);
                nb::gil_scoped_release release;
                s.feed(p, n);
            })
        .def("rows", &Session::rows)
        .def("take_bbo", &Session::take_bbo)
        .def("take_trades", &Session::take_trades)
        .def("take_messages", &Session::take_messages)
        .def("take_events", &Session::take_events)
        .def("take_symbols", &Session::take_symbols)
        .def("mpids", &Session::mpids)
        .def("stats", &Session::stats);
}
