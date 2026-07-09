#include <itch/parser.hpp>
#include <itch/stream.hpp>

#include "check.hpp"
#include "encode.hpp"
#include "synthetic.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace itch;

namespace {

struct Counter {
    std::uint64_t adds = 0;
    std::uint64_t deletes = 0;
    std::uint64_t last_ref = 0;

    void on_add(const AddOrder& m) {
        ++adds;
        last_ref = m.ref;
    }
    void on_delete(const OrderDelete& m) {
        ++deletes;
        last_ref = m.ref;
    }
};

std::vector<std::byte> synthetic_feed(std::uint64_t seed, int steps, bool finish) {
    synth::Generator g(seed, 9);
    std::vector<std::byte> buf;
    g.preamble(buf);
    for (int i = 0; i < steps; ++i) g.step(buf);
    if (finish) g.finish(buf);
    return buf;
}

bool same_result(const ParseResult& a, const ParseResult& b) {
    return a.consumed == b.consumed && a.messages == b.messages && a.unknown == b.unknown &&
           a.malformed == b.malformed && a.end_of_session == b.end_of_session;
}

void byte_at_a_time() {
    const auto buf = synthetic_feed(3, 5000, true);
    const auto whole = scan(buf);

    Counter c;
    StreamParser<Counter> sp(c);
    for (const std::byte byte : buf) sp.feed(std::span(&byte, 1));
    CHECK(same_result(sp.result(), whole));
    CHECK(sp.pending_bytes() == 0);
}

void random_chunks_match_one_shot() {
    const auto buf = synthetic_feed(17, 20000, true);

    Counter one;
    const auto whole = parse(buf, one);

    synth::Rng rng{29};
    for (int round = 0; round < 20; ++round) {
        Counter c;
        StreamParser<Counter> sp(c);
        std::size_t pos = 0;
        while (pos < buf.size()) {
            const std::size_t take =
                std::min<std::size_t>(1 + rng.below(4096), buf.size() - pos);
            sp.feed(std::span(buf.data() + pos, take));
            pos += take;
        }
        CHECK(same_result(sp.result(), whole));
        CHECK(c.adds == one.adds);
        CHECK(c.deletes == one.deletes);
        CHECK(c.last_ref == one.last_ref);
    }
}

void frame_boundary_chunks() {
    std::vector<std::byte> buf;
    enc::add_order(buf, 7, 1, 5001, 'B', 10, "MSFT", 4'210'000);
    enc::order_delete(buf, 7, 2, 5001);

    Counter c;
    StreamParser<Counter> sp(c);
    sp.feed(std::span(buf.data(), 38));
    CHECK(sp.result().messages == 1);
    CHECK(sp.pending_bytes() == 0);
    sp.feed(std::span(buf.data() + 38, buf.size() - 38));
    CHECK(sp.result().messages == 2);
    CHECK(c.adds == 1 && c.deletes == 1);
}

void split_inside_length_prefix() {
    std::vector<std::byte> buf;
    enc::order_delete(buf, 7, 1, 42);
    enc::order_delete(buf, 7, 2, 43);

    Counter c;
    StreamParser<Counter> sp(c);
    sp.feed(std::span(buf.data(), std::size_t{1}));
    CHECK(sp.result().messages == 0);
    CHECK(sp.pending_bytes() == 1);
    sp.feed(std::span(buf.data() + 1, std::size_t{1}));
    CHECK(sp.result().messages == 0);
    sp.feed(std::span(buf.data() + 2, buf.size() - 2));
    CHECK(sp.result().messages == 2);
    CHECK(c.deletes == 2 && c.last_ref == 43);
}

void end_of_session_stops_consuming() {
    std::vector<std::byte> buf;
    enc::order_delete(buf, 7, 1, 42);
    enc::end_of_session(buf);
    enc::order_delete(buf, 7, 2, 43);

    Counter c;
    StreamParser<Counter> sp(c);
    // split so the end-of-session marker itself straddles a chunk boundary
    const std::size_t cut = 22;
    sp.feed(std::span(buf.data(), cut));
    sp.feed(std::span(buf.data() + cut, buf.size() - cut));
    CHECK(sp.result().end_of_session);
    CHECK(sp.result().messages == 1);
    CHECK(c.deletes == 1);

    sp.feed(buf);
    CHECK(sp.result().messages == 1);
}

void mutated_chunked_matches_mutated_one_shot() {
    auto pristine = synthetic_feed(11, 3000, true);
    synth::Rng rng{5};
    for (int round = 0; round < 50; ++round) {
        std::vector<std::byte> buf = pristine;
        for (int hits = 0; hits < 25; ++hits)
            buf[rng.below(static_cast<std::uint32_t>(buf.size()))] =
                static_cast<std::byte>(rng.below(256));

        const auto whole = scan(buf);
        NoopHandler h;
        StreamParser<NoopHandler> sp(h);
        std::size_t pos = 0;
        while (pos < buf.size()) {
            const std::size_t take =
                std::min<std::size_t>(1 + rng.below(700), buf.size() - pos);
            sp.feed(std::span(buf.data() + pos, take));
            pos += take;
        }
        // a one-shot parse stops at a truncated tail; the stream keeps it buffered
        CHECK(sp.result().messages == whole.messages);
        CHECK(sp.result().unknown == whole.unknown);
        CHECK(sp.result().malformed == whole.malformed);
        CHECK(sp.result().end_of_session == whole.end_of_session);
        CHECK(sp.result().consumed == whole.consumed);
        CHECK(sp.pending_bytes() <= 2 + 0xffffu);
    }
}

}  // namespace

int main() {
    byte_at_a_time();
    random_chunks_match_one_shot();
    frame_boundary_chunks();
    split_inside_length_prefix();
    end_of_session_stops_consuming();
    mutated_chunked_matches_mutated_one_shot();
    RUN_END();
}
