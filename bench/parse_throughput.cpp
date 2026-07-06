#include <itch/messages.hpp>
#include <itch/parser.hpp>

#include "slurp.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>

namespace {

struct Checksum {
    std::uint64_t acc = 0;

    void on_system_event(const itch::SystemEvent& m) { acc += static_cast<unsigned char>(m.event); }
    void on_stock_directory(const itch::StockDirectory& m) { acc += m.round_lot_size; }
    void on_add(const itch::AddOrder& m) {
        acc += m.ref + static_cast<std::uint64_t>(m.price.raw()) + m.shares;
    }
    void on_execute(const itch::OrderExecuted& m) { acc += m.ref + m.shares + m.match; }
    void on_execute_price(const itch::OrderExecutedPrice& m) {
        acc += m.ref + static_cast<std::uint64_t>(m.price.raw());
    }
    void on_cancel(const itch::OrderCancel& m) { acc += m.ref + m.shares; }
    void on_delete(const itch::OrderDelete& m) { acc += m.ref; }
    void on_replace(const itch::OrderReplace& m) {
        acc += m.old_ref + m.new_ref + static_cast<std::uint64_t>(m.price.raw());
    }
    void on_trade(const itch::Trade& m) { acc += m.match + m.shares; }
};

template <typename F>
void bench_tier(const char* name, std::span<const std::byte> data, int reps, F&& body) {
    double best = 0;
    std::uint64_t msgs = 0;
    for (int i = 0; i < reps; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const itch::ParseResult r = body();
        const double dt =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        msgs = r.messages + r.unknown + r.malformed;
        const double rate = msgs / dt / 1e6;
        if (rate > best) best = rate;
        std::printf("%-12s rep %d: %6.1f M msg/s (%.0f MB/s)\n", name, i + 1, rate,
                    data.size() / dt / 1e6);
    }
    std::printf("%-12s best:  %6.1f M msg/s, %.1f ns/msg\n\n", name, best,
                1e3 / best);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: parse_throughput <itch50-file>\n");
        return 2;
    }
    try {
        const auto data = bench::slurp(argv[1]);
        std::printf("file: %s (%.2f GB in memory)\n\n", argv[1], data.size() / 1e9);

        bench_tier("framing", data, 3, [&] { return itch::scan(data); });

        Checksum h;
        bench_tier("decode", data, 3, [&] { return itch::parse(data, h); });
        std::printf("checksum %llu\n", static_cast<unsigned long long>(h.acc));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
