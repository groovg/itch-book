#include <itch/book_manager.hpp>
#include <itch/parser.hpp>

#include "slurp.hpp"

#include <tsclat/clock.hpp>
#include <tsclat/histogram.hpp>
#include <tsclat/tsc.hpp>

#include <cstdint>
#include <cstdio>
#include <exception>

namespace {

template <typename F>
std::uint64_t timed(F&& f) {
    const std::uint64_t t0 = tsclat::tsc_begin();
    f();
    return tsclat::tsc_end() - t0;
}

struct Timed {
    itch::BookManager<> mgr;
    tsclat::Histogram add, reduce, del, rep;

    void on_stock_directory(const itch::StockDirectory& m) { mgr.on_stock_directory(m); }
    void on_add(const itch::AddOrder& m) { add.record(timed([&] { mgr.on_add(m); })); }
    void on_execute(const itch::OrderExecuted& m) {
        reduce.record(timed([&] { mgr.on_execute(m); }));
    }
    void on_execute_price(const itch::OrderExecutedPrice& m) {
        reduce.record(timed([&] { mgr.on_execute_price(m); }));
    }
    void on_cancel(const itch::OrderCancel& m) {
        reduce.record(timed([&] { mgr.on_cancel(m); }));
    }
    void on_delete(const itch::OrderDelete& m) { del.record(timed([&] { mgr.on_delete(m); })); }
    void on_replace(const itch::OrderReplace& m) { rep.record(timed([&] { mgr.on_replace(m); })); }
};

void print_row(const char* name, const tsclat::Histogram& h) {
    const auto& c = tsclat::TscClock::instance();
    std::printf("%-8s %12llu %8.0f %7.0f %7.0f %7.0f %8.0f %9.0f\n", name,
                static_cast<unsigned long long>(h.count()), c.to_ns(h.value_at_quantile(0.50)),
                c.to_ns(h.value_at_quantile(0.90)), c.to_ns(h.value_at_quantile(0.99)),
                c.to_ns(h.value_at_quantile(0.999)), c.to_ns(h.value_at_quantile(0.9999)),
                c.to_ns(h.max()));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: apply_latency <itch50-file>\n");
        return 2;
    }
    try {
        const auto data = bench::slurp(argv[1]);
        const auto& clock = tsclat::TscClock::instance();
        std::printf("invariant tsc: %s, %.4f ns/tick\n", tsclat::has_invariant_tsc() ? "yes" : "no",
                    clock.ns_per_tick());

        tsclat::Histogram floor;
        for (int i = 0; i < 200'000; ++i) floor.record(timed([] {}));

        Timed h;
        const auto r = itch::parse(data, h);
        std::printf("messages %llu (missing %llu dup %llu)\n\n",
                    static_cast<unsigned long long>(r.messages),
                    static_cast<unsigned long long>(h.mgr.stats().missing_ref),
                    static_cast<unsigned long long>(h.mgr.stats().dup_ref));

        std::printf("%-8s %12s %8s %7s %7s %7s %8s %9s\n", "op", "count", "p50", "p90", "p99",
                    "p99.9", "p99.99", "max");
        print_row("floor", floor);
        print_row("add", h.add);
        print_row("reduce", h.reduce);
        print_row("delete", h.del);
        print_row("replace", h.rep);
        std::printf("\nns, uncorrected; includes the timestamp-pair floor shown above\n");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
