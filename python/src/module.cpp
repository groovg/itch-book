#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>

#include <cstdint>
#include <string>
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
};

class Session {
    struct Sink {
        Session* s;
        void operator()(std::uint16_t locate, const itch::Bbo& b) const { s->emit_bbo(locate, b); }
    };
    using Manager = itch::BookManager<Sink>;

    struct Handler {
        Session& s;
        Manager& mgr;

        void on_system_event(const itch::SystemEvent& m) {
            s.stamp(m.hdr);
            s.last_event_ = m.event;
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
            mgr.on_add(m);
        }
        void on_execute(const itch::OrderExecuted& m) {
            s.stamp(m.hdr);
            mgr.on_execute(m);
        }
        void on_execute_price(const itch::OrderExecutedPrice& m) {
            s.stamp(m.hdr);
            mgr.on_execute_price(m);
        }
        void on_cancel(const itch::OrderCancel& m) {
            s.stamp(m.hdr);
            mgr.on_cancel(m);
        }
        void on_delete(const itch::OrderDelete& m) {
            s.stamp(m.hdr);
            mgr.on_delete(m);
        }
        void on_replace(const itch::OrderReplace& m) {
            s.stamp(m.hdr);
            mgr.on_replace(m);
        }
        void on_trade(const itch::Trade& m) { s.stamp(m.hdr); }
        void on_cross(const itch::CrossTrade& m) { s.stamp(m.hdr); }
        void on_broken(const itch::BrokenTrade& m) { s.stamp(m.hdr); }
        void on_other(char) { ++s.seq_; }
    };

  public:
    Session() : mgr_(Sink{this}), handler_{*this, mgr_}, parser_(handler_) {}
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void reserve(std::size_t rows) {
        reserve_ = rows;
        for_each_column([rows](auto& v) { v.reserve(rows); });
    }

    void feed(const std::uint8_t* p, std::size_t n) {
        parser_.feed({reinterpret_cast<const std::byte*>(p), n});
    }

    std::size_t bbo_rows() const { return bbo_.ts.size(); }
    std::size_t symbol_rows() const { return symbols_.size(); }

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
    template <typename F>
    void for_each_column(F f) {
        f(bbo_.ts);
        f(bbo_.seq);
        f(bbo_.locate);
        f(bbo_.bid_px);
        f(bbo_.bid_sz);
        f(bbo_.bid_ct);
        f(bbo_.ask_px);
        f(bbo_.ask_sz);
        f(bbo_.ask_ct);
    }

    template <typename T>
    nb::ndarray<nb::numpy, T, nb::ndim<1>> take(std::vector<T>& v) {
        auto* heap = new std::vector<T>(std::move(v));
        v.reserve(reserve_);
        nb::capsule owner(heap, [](void* p) noexcept { delete static_cast<std::vector<T>*>(p); });
        return nb::ndarray<nb::numpy, T, nb::ndim<1>>(heap->data(), {heap->size()}, owner);
    }

    void stamp(const itch::Header& h) {
        ts_ = h.timestamp;
        ++seq_;
    }

    void emit_bbo(std::uint16_t locate, const itch::Bbo& b) {
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

    Manager mgr_;
    Handler handler_;
    itch::StreamParser<Handler> parser_;
    BboColumns bbo_;
    std::vector<itch::StockDirectory> symbols_;
    std::size_t reserve_ = 0;
    std::uint64_t ts_ = 0;
    std::uint64_t seq_ = 0;
    char last_event_ = ' ';
};

}  // namespace

NB_MODULE(_core, m) {
    nb::class_<Session>(m, "Session")
        .def(nb::init<>())
        .def("reserve", &Session::reserve)
        .def(
            "feed",
            [](Session& s, nb::ndarray<const std::uint8_t, nb::ndim<1>, nb::c_contig, nb::device::cpu> buf) {
                const std::uint8_t* p = buf.data();
                const std::size_t n = buf.shape(0);
                nb::gil_scoped_release release;
                s.feed(p, n);
            })
        .def("bbo_rows", &Session::bbo_rows)
        .def("symbol_rows", &Session::symbol_rows)
        .def("take_bbo", &Session::take_bbo)
        .def("take_symbols", &Session::take_symbols)
        .def("stats", &Session::stats);
}
