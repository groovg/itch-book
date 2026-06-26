#include <itch/messages.hpp>
#include <itch/parser.hpp>
#include <itch/wire.hpp>

#include "check.hpp"

#include <vector>

using namespace itch;

namespace {

std::byte b(unsigned v) { return static_cast<std::byte>(v); }

void frame(std::vector<std::byte>& out, char type, std::size_t body_len) {
    out.push_back(b(static_cast<unsigned>(body_len >> 8)));
    out.push_back(b(static_cast<unsigned>(body_len & 0xff)));
    out.push_back(b(static_cast<unsigned>(type)));
    for (std::size_t i = 1; i < body_len; ++i) out.push_back(b(0));
}

void wire_loads() {
    const std::byte two[] = {b(0x12), b(0x34)};
    CHECK(wire::u16(two) == 0x1234);

    const std::byte four[] = {b(0xde), b(0xad), b(0xbe), b(0xef)};
    CHECK(wire::u32(four) == 0xdeadbeefu);

    const std::byte eight[] = {b(0x01), b(0x02), b(0x03), b(0x04),
                               b(0x05), b(0x06), b(0x07), b(0x08)};
    CHECK(wire::u64(eight) == 0x0102030405060708ull);

    const std::byte six[] = {b(0x00), b(0x00), b(0x0a), b(0x2d), b(0x27), b(0xa1)};
    CHECK(wire::u48(six) == 0x0a2d27a1ull);

    const std::byte max6[] = {b(0xff), b(0xff), b(0xff), b(0xff), b(0xff), b(0xff)};
    CHECK(wire::u48(max6) == 0xffffffffffffull);
}

void wire_alpha() {
    const std::byte stock[] = {b('A'), b('A'), b('P'), b('L'),
                               b(' '), b(' '), b(' '), b(' ')};
    auto a = wire::alpha<8>(stock);
    CHECK(a.view() == "AAPL");

    const std::byte blank[] = {b(' '), b(' '), b(' '), b(' ')};
    CHECK(wire::alpha<4>(blank).view().empty());

    const std::byte full[] = {b('Z'), b('X'), b('Z'), b('Z'),
                              b('Z'), b('Z'), b('Z'), b('T')};
    CHECK(wire::alpha<8>(full).view() == "ZXZZZZZT");
}

void length_table() {
    CHECK(wire_length('S') == 12);
    CHECK(wire_length('R') == 39);
    CHECK(wire_length('H') == 25);
    CHECK(wire_length('Y') == 20);
    CHECK(wire_length('L') == 26);
    CHECK(wire_length('V') == 35);
    CHECK(wire_length('W') == 12);
    CHECK(wire_length('K') == 28);
    CHECK(wire_length('J') == 35);
    CHECK(wire_length('h') == 21);
    CHECK(wire_length('A') == 36);
    CHECK(wire_length('F') == 40);
    CHECK(wire_length('E') == 31);
    CHECK(wire_length('C') == 36);
    CHECK(wire_length('X') == 23);
    CHECK(wire_length('D') == 19);
    CHECK(wire_length('U') == 35);
    CHECK(wire_length('P') == 44);
    CHECK(wire_length('Q') == 40);
    CHECK(wire_length('B') == 19);
    CHECK(wire_length('I') == 50);
    CHECK(wire_length('N') == 20);
    CHECK(wire_length('O') == 48);
    CHECK(wire_length('z') == 0);
    CHECK(wire_length('\0') == 0);
}

void scan_frames() {
    std::vector<std::byte> buf;
    frame(buf, 'S', 12);
    frame(buf, 'R', 39);
    frame(buf, 'A', 36);
    auto r = scan(buf);
    CHECK(r.messages == 3);
    CHECK(r.unknown == 0);
    CHECK(r.malformed == 0);
    CHECK(r.consumed == buf.size());
    CHECK(!r.end_of_session);
}

void scan_end_of_session() {
    std::vector<std::byte> buf;
    frame(buf, 'S', 12);
    buf.push_back(b(0));
    buf.push_back(b(0));
    frame(buf, 'A', 36);
    auto r = scan(buf);
    CHECK(r.messages == 1);
    CHECK(r.end_of_session);
    CHECK(r.consumed == 14 + 2);
}

void scan_truncated_tail() {
    std::vector<std::byte> buf;
    frame(buf, 'D', 19);
    frame(buf, 'A', 36);
    const std::size_t full = buf.size();
    for (std::size_t cut = 1; cut < 38; ++cut) {
        auto r = scan(std::span(buf.data(), full - cut));
        CHECK(r.messages == 1);
        CHECK(r.consumed == 21);
        CHECK(!r.end_of_session);
    }
    auto r1 = scan(std::span(buf.data(), std::size_t{1}));
    CHECK(r1.messages == 0);
    CHECK(r1.consumed == 0);
    auto r0 = scan(std::span(buf.data(), std::size_t{0}));
    CHECK(r0.messages == 0);
    CHECK(r0.consumed == 0);
}

void scan_unknown_and_malformed() {
    std::vector<std::byte> buf;
    frame(buf, 'z', 17);
    frame(buf, 'A', 20);
    frame(buf, 'D', 19);
    auto r = scan(buf);
    CHECK(r.messages == 1);
    CHECK(r.unknown == 1);
    CHECK(r.malformed == 1);
    CHECK(r.consumed == buf.size());
}

}  // namespace

int main() {
    wire_loads();
    wire_alpha();
    length_table();
    scan_frames();
    scan_end_of_session();
    scan_truncated_tail();
    scan_unknown_and_malformed();
    RUN_END();
}
