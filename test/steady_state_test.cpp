#include <itch/book_manager.hpp>
#include <itch/parser.hpp>

#include "check.hpp"
#include "synthetic.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

namespace {
std::atomic<std::uint64_t> g_allocs{0};
}

void* operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc{};
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main() {
    synth::Generator g(11, 20);
    std::vector<std::byte> warmup;
    std::vector<std::byte> steady;
    g.preamble(warmup);
    for (int i = 0; i < 400'000; ++i) g.step(warmup);
    for (int i = 0; i < 400'000; ++i) g.step(steady);

    itch::BookManager<> mgr;
    mgr.reserve(24, 8192);
    mgr.orders().reserve(std::uint64_t{1} << 23, 200'000, 512);
    const auto w = itch::parse(warmup, mgr);
    CHECK(w.messages > 0);

    const std::uint64_t before = g_allocs.load();
    const auto r = itch::parse(steady, mgr);
    const std::uint64_t during = g_allocs.load() - before;

    CHECK(r.messages > 0);
    CHECK(mgr.stats().adds > 0);
    if (during != 0)
        std::fprintf(stderr, "steady-state allocations: %llu\n",
                     static_cast<unsigned long long>(during));
    CHECK(during == 0);
    RUN_END();
}
