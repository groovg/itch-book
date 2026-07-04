#include <itch/book_manager.hpp>
#include <string>
#include <itch/mapped_file.hpp>
#include <itch/messages.hpp>
#include <itch/parser.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

namespace {

struct Counts {
    std::array<std::uint64_t, 256> by_type{};

    void bump(char t) { ++by_type[static_cast<unsigned char>(t)]; }

    void on_system_event(const itch::SystemEvent&) { bump('S'); }
    void on_stock_directory(const itch::StockDirectory&) { bump('R'); }
    void on_add(const itch::AddOrder& m) { bump(m.attributed ? 'F' : 'A'); }
    void on_execute(const itch::OrderExecuted&) { bump('E'); }
    void on_execute_price(const itch::OrderExecutedPrice&) { bump('C'); }
    void on_cancel(const itch::OrderCancel&) { bump('X'); }
    void on_delete(const itch::OrderDelete&) { bump('D'); }
    void on_replace(const itch::OrderReplace&) { bump('U'); }
    void on_trade(const itch::Trade&) { bump('P'); }
    void on_other(char t) { bump(t); }
};

struct BboCount {
    std::uint64_t* n;
    void operator()(std::uint16_t, const itch::Bbo&) const { ++*n; }
};

struct App {
    std::uint64_t bbo_updates = 0;
    itch::BookManager<BboCount> mgr{BboCount{&bbo_updates}};
    std::vector<std::uint64_t> adds_by_locate;

    void on_stock_directory(const itch::StockDirectory& m) { mgr.on_stock_directory(m); }
    void on_add(const itch::AddOrder& m) {
        if (m.hdr.locate >= adds_by_locate.size()) adds_by_locate.resize(m.hdr.locate + 1);
        ++adds_by_locate[m.hdr.locate];
        mgr.on_add(m);
    }
    void on_execute(const itch::OrderExecuted& m) { mgr.on_execute(m); }
    void on_execute_price(const itch::OrderExecutedPrice& m) { mgr.on_execute_price(m); }
    void on_cancel(const itch::OrderCancel& m) { mgr.on_cancel(m); }
    void on_delete(const itch::OrderDelete& m) { mgr.on_delete(m); }
    void on_replace(const itch::OrderReplace& m) { mgr.on_replace(m); }
};

void print_result(const itch::ParseResult& r, std::size_t file_size, double dt) {
    const std::uint64_t total = r.messages + r.unknown + r.malformed;
    std::printf("\nmessages   %llu\n", static_cast<unsigned long long>(r.messages));
    if (r.unknown) std::printf("unknown    %llu\n", static_cast<unsigned long long>(r.unknown));
    if (r.malformed)
        std::printf("malformed  %llu\n", static_cast<unsigned long long>(r.malformed));
    if (!r.end_of_session) std::printf("warning: no end-of-session marker\n");
    if (r.consumed != file_size)
        std::printf("warning: %llu trailing bytes not consumed\n",
                    static_cast<unsigned long long>(file_size - r.consumed));
    std::printf("elapsed    %.3f s\n", dt);
    std::printf("rate       %.1f M msg/s, %.0f MB/s\n", total / dt / 1e6,
                file_size / dt / 1e6);
}

int run_counts(const itch::MappedFile& file) {
    Counts h;
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = itch::parse(file.bytes(), h);
    const double dt =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::printf("%-5s %14s\n", "type", "count");
    for (unsigned t = 0; t < 256; ++t) {
        if (h.by_type[t] != 0)
            std::printf("%-5c %14llu\n", static_cast<char>(t),
                        static_cast<unsigned long long>(h.by_type[t]));
    }
    print_result(r, file.size(), dt);
    return 0;
}

int run_book(const itch::MappedFile& file) {
    App app;
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = itch::parse(file.bytes(), app);
    const double dt =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const itch::Stats& s = app.mgr.stats();
    std::printf("adds       %llu\n", static_cast<unsigned long long>(s.adds));
    std::printf("executes   %llu\n", static_cast<unsigned long long>(s.executes));
    std::printf("cancels    %llu\n", static_cast<unsigned long long>(s.cancels));
    std::printf("deletes    %llu\n", static_cast<unsigned long long>(s.deletes));
    std::printf("replaces   %llu\n", static_cast<unsigned long long>(s.replaces));
    std::printf("missing    %llu\n", static_cast<unsigned long long>(s.missing_ref));
    std::printf("dup refs   %llu\n", static_cast<unsigned long long>(s.dup_ref));
    std::printf("rejected   %llu\n", static_cast<unsigned long long>(s.rejected));
    std::printf("clamped    %llu\n", static_cast<unsigned long long>(s.clamped));
    std::printf("bbo moves  %llu\n", static_cast<unsigned long long>(app.bbo_updates));

    std::size_t crossed = 0;
    for (std::size_t i = 0; i < app.mgr.book_count(); ++i)
        if (app.mgr.book(static_cast<std::uint16_t>(i))->crossed()) ++crossed;
    std::printf("books      %zu (%zu crossed at close)\n", app.mgr.book_count(), crossed);
    std::printf("live       %llu orders, %zu resident pages (%zu spare)\n",
                static_cast<unsigned long long>(app.mgr.orders().live_orders()),
                app.mgr.orders().resident_pages(), app.mgr.orders().spare_pages());

    std::vector<std::uint16_t> locates(app.adds_by_locate.size());
    for (std::size_t i = 0; i < locates.size(); ++i) locates[i] = static_cast<std::uint16_t>(i);
    std::sort(locates.begin(), locates.end(), [&](std::uint16_t a, std::uint16_t b) {
        return app.adds_by_locate[a] > app.adds_by_locate[b];
    });
    std::printf("\n%-9s %14s %26s x %-26s\n", "symbol", "adds", "bid", "ask");
    for (std::size_t i = 0; i < locates.size() && i < 5; ++i) {
        const std::uint16_t loc = locates[i];
        const itch::Bbo q = app.mgr.bbo(loc);
        const std::string sym{app.mgr.symbol(loc).view()};
        const std::string bid = q.has_bid ? q.bid.price.to_string() : "-";
        const std::string ask = q.has_ask ? q.ask.price.to_string() : "-";
        std::printf("%-9s %14llu %15s (%8llu) x %-15s (%8llu)\n", sym.c_str(),
                    static_cast<unsigned long long>(app.adds_by_locate[loc]), bid.c_str(),
                    static_cast<unsigned long long>(q.has_bid ? q.bid.shares : 0), ask.c_str(),
                    static_cast<unsigned long long>(q.has_ask ? q.ask.shares : 0));
    }
    print_result(r, file.size(), dt);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3 || (argc == 3 && std::strcmp(argv[2], "--book") != 0)) {
        std::fprintf(stderr, "usage: itch-replay <itch50-file> [--book]\n");
        return 2;
    }
    try {
        itch::MappedFile file(argv[1]);
        return argc == 3 ? run_book(file) : run_counts(file);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
