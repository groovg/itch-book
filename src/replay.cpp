#include <itch/mapped_file.hpp>
#include <itch/messages.hpp>
#include <itch/parser.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>

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

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: itch-replay <itch50-file>\n");
        return 2;
    }
    try {
        itch::MappedFile file(argv[1]);
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
        const std::uint64_t total = r.messages + r.unknown + r.malformed;
        std::printf("\nmessages   %llu\n", static_cast<unsigned long long>(r.messages));
        if (r.unknown) std::printf("unknown    %llu\n", static_cast<unsigned long long>(r.unknown));
        if (r.malformed)
            std::printf("malformed  %llu\n", static_cast<unsigned long long>(r.malformed));
        if (!r.end_of_session) std::printf("warning: no end-of-session marker\n");
        if (r.consumed != file.size())
            std::printf("warning: %llu trailing bytes not consumed\n",
                        static_cast<unsigned long long>(file.size() - r.consumed));
        std::printf("elapsed    %.3f s\n", dt);
        std::printf("rate       %.1f M msg/s, %.0f MB/s\n", total / dt / 1e6,
                    file.size() / dt / 1e6);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
