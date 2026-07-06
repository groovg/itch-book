#include <itch/book_manager.hpp>
#include <itch/parser.hpp>

#include "reference.hpp"
#include "rss.hpp"
#include "slurp.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>

namespace {

void report(const char* name, const itch::ParseResult& r, double dt, std::size_t base) {
    const std::uint64_t msgs = r.messages + r.unknown + r.malformed;
    const std::size_t peak = bench::peak_rss_bytes();
    std::printf("%-8s %6.1f M msg/s  %5.1f ns/msg  structures ~%.2f GB\n", name,
                msgs / dt / 1e6, dt * 1e9 / msgs,
                peak > base ? (peak - base) / 1e9 : 0.0);
}

template <typename Mgr>
void run_manager(const char* name, std::span<const std::byte> data, std::size_t base) {
    Mgr mgr;
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = itch::parse(data, mgr);
    const double dt =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    report(name, r, dt, base);
    const itch::Stats& s = mgr.stats();
    if (s.missing_ref || s.dup_ref || s.rejected)
        std::printf("  anomalies: missing %llu dup %llu rejected %llu\n",
                    static_cast<unsigned long long>(s.missing_ref),
                    static_cast<unsigned long long>(s.dup_ref),
                    static_cast<unsigned long long>(s.rejected));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: book_throughput <itch50-file> <inline|pooled|hash|naive>\n");
        return 2;
    }
    try {
        const auto data = bench::slurp(argv[1]);
        const std::size_t base = bench::peak_rss_bytes();
        const char* v = argv[2];

        if (std::strcmp(v, "inline") == 0) {
            run_manager<itch::BookManager<>>(v, data, base);
        } else if (std::strcmp(v, "pooled") == 0) {
            run_manager<itch::BookManager<std::nullptr_t, itch::Book<itch::PooledOrderStore>>>(
                v, data, base);
        } else if (std::strcmp(v, "hash") == 0) {
            run_manager<itch::BookManager<std::nullptr_t, itch::Book<itch::HashOrderStore>>>(
                v, data, base);
        } else if (std::strcmp(v, "naive") == 0) {
            refimpl::Reference ref;
            const auto t0 = std::chrono::steady_clock::now();
            const auto r = itch::parse(data, ref);
            const double dt =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            report(v, r, dt, base);
        } else {
            std::fprintf(stderr, "unknown variant: %s\n", v);
            return 2;
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
