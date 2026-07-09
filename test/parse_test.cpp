#include <itch/book_manager.hpp>
#include <itch/messages.hpp>
#include <itch/parser.hpp>
#include <itch/wire.hpp>

#include "check.hpp"
#include "encode.hpp"
#include "synthetic.hpp"

#include <optional>
#include <span>
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

struct Recorder {
    std::optional<SystemEvent> sys;
    std::optional<StockDirectory> dir;
    std::vector<AddOrder> adds;
    std::optional<OrderExecuted> exec;
    std::optional<OrderExecutedPrice> exec_px;
    std::optional<OrderCancel> cancel;
    std::optional<OrderDelete> del;
    std::optional<OrderReplace> replace;
    std::optional<Trade> trade;
    std::optional<TradingAction> action;
    std::optional<CrossTrade> cross;
    std::optional<BrokenTrade> broken;

    void on_system_event(const SystemEvent& m) { sys = m; }
    void on_stock_directory(const StockDirectory& m) { dir = m; }
    void on_add(const AddOrder& m) { adds.push_back(m); }
    void on_execute(const OrderExecuted& m) { exec = m; }
    void on_execute_price(const OrderExecutedPrice& m) { exec_px = m; }
    void on_cancel(const OrderCancel& m) { cancel = m; }
    void on_delete(const OrderDelete& m) { del = m; }
    void on_replace(const OrderReplace& m) { replace = m; }
    void on_trade(const Trade& m) { trade = m; }
    void on_trading_action(const TradingAction& m) { action = m; }
    void on_cross(const CrossTrade& m) { cross = m; }
    void on_broken(const BrokenTrade& m) { broken = m; }
};

void decode_all_types() {
    std::vector<std::byte> buf;
    enc::system_event(buf, 34200'000'000'000ull, 'Q');
    enc::stock_directory(buf, 42, 34200'000'000'001ull, "AAPL", 100);
    enc::add_order(buf, 42, 34200'000'000'002ull, 1001, 'B', 300, "AAPL", 1'857'400);
    enc::add_order_mpid(buf, 42, 34200'000'000'003ull, 1002, 'S', 200, "AAPL", 1'857'900,
                        "JPMC");
    enc::order_executed(buf, 42, 34200'000'000'004ull, 1001, 100, 555001);
    enc::order_executed_price(buf, 42, 34200'000'000'005ull, 1001, 50, 555002, false,
                              1'857'500);
    enc::order_cancel(buf, 42, 34200'000'000'006ull, 1002, 25);
    enc::order_delete(buf, 42, 34200'000'000'007ull, 1002);
    enc::order_replace(buf, 42, 34200'000'000'008ull, 1001, 1003, 400, 1'856'000);
    enc::trade(buf, 42, 34200'000'000'009ull, 75, "AAPL", 1'857'200, 555003);
    enc::trading_action(buf, 42, 34200'000'000'010ull, "AAPL", 'H', "LUDP");
    enc::cross_trade(buf, 42, 34200'000'000'011ull, 120'000, "AAPL", 1'857'000, 555004, 'O');
    enc::broken_trade(buf, 42, 34200'000'000'012ull, 555003);
    enc::end_of_session(buf);

    Recorder h;
    auto r = parse(buf, h);
    CHECK(r.messages == 13);
    CHECK(r.unknown == 0);
    CHECK(r.malformed == 0);
    CHECK(r.end_of_session);
    CHECK(r.consumed == buf.size());

    CHECK(h.sys && h.sys->event == 'Q');
    CHECK(h.sys->hdr.locate == 0);
    CHECK(h.sys->hdr.timestamp == 34200'000'000'000ull);

    CHECK(h.dir && h.dir->stock.view() == "AAPL");
    CHECK(h.dir->round_lot_size == 100);
    CHECK(h.dir->market_category == 'Q');
    CHECK(h.dir->authenticity == 'P');
    CHECK(h.dir->luld_tier == '1');

    CHECK(h.adds.size() == 2);
    CHECK(h.adds[0].ref == 1001);
    CHECK(h.adds[0].side == Side::Buy);
    CHECK(h.adds[0].shares == 300);
    CHECK(h.adds[0].stock.view() == "AAPL");
    CHECK(h.adds[0].price.raw() == 1'857'400);
    CHECK(!h.adds[0].attributed);
    CHECK(h.adds[0].mpid.view().empty());
    CHECK(h.adds[1].ref == 1002);
    CHECK(h.adds[1].side == Side::Sell);
    CHECK(h.adds[1].attributed);
    CHECK(h.adds[1].mpid.view() == "JPMC");

    CHECK(h.exec && h.exec->ref == 1001);
    CHECK(h.exec->shares == 100);
    CHECK(h.exec->match == 555001);

    CHECK(h.exec_px && h.exec_px->ref == 1001);
    CHECK(h.exec_px->shares == 50);
    CHECK(!h.exec_px->printable);
    CHECK(h.exec_px->price.raw() == 1'857'500);

    CHECK(h.cancel && h.cancel->ref == 1002);
    CHECK(h.cancel->shares == 25);

    CHECK(h.del && h.del->ref == 1002);

    CHECK(h.replace && h.replace->old_ref == 1001);
    CHECK(h.replace->new_ref == 1003);
    CHECK(h.replace->shares == 400);
    CHECK(h.replace->price.raw() == 1'856'000);

    CHECK(h.trade && h.trade->shares == 75);
    CHECK(h.trade->stock.view() == "AAPL");
    CHECK(h.trade->price.raw() == 1'857'200);
    CHECK(h.trade->match == 555003);
    CHECK(h.trade->hdr.locate == 42);

    CHECK(h.action && h.action->stock.view() == "AAPL");
    CHECK(h.action->state == 'H');
    CHECK(h.action->reason.view() == "LUDP");

    CHECK(h.cross && h.cross->shares == 120'000);
    CHECK(h.cross->stock.view() == "AAPL");
    CHECK(h.cross->price.raw() == 1'857'000);
    CHECK(h.cross->match == 555004);
    CHECK(h.cross->cross_type == 'O');

    CHECK(h.broken && h.broken->match == 555003);
    CHECK(h.broken->hdr.timestamp == 34200'000'000'012ull);
}

void manager_trading_state_and_prints() {
    std::vector<std::byte> halted;
    enc::stock_directory(halted, 3, 1, "TEST", 100);
    enc::trading_action(halted, 3, 2, "TEST", 'H', "T1  ");
    enc::add_order(halted, 3, 3, 900, 'B', 100, "TEST", 500'000);
    enc::add_order(halted, 3, 4, 901, 'S', 80, "TEST", 510'000);

    std::vector<std::byte> trading;
    enc::trading_action(trading, 3, 5, "TEST", 'T', "    ");
    enc::order_executed(trading, 3, 6, 900, 30, 71);
    enc::order_executed_price(trading, 3, 7, 900, 20, 72, true, 495'000);
    enc::order_executed_price(trading, 3, 8, 900, 10, 73, false, 495'000);
    enc::trade(trading, 3, 9, 75, "TEST", 505'000, 74);
    enc::cross_trade(trading, 3, 10, 120'000, "TEST", 502'000, 75, 'O');
    enc::broken_trade(trading, 3, 11, 74);

    std::vector<TradePrint> prints;
    auto sink = [&prints](const TradePrint& p) { prints.push_back(p); };
    BookManager<std::nullptr_t, Book<>, decltype(sink)> mgr(nullptr, sink);

    parse(halted, mgr);
    CHECK(mgr.trading_state(3) == 'H');
    CHECK(mgr.trading_state(4) == ' ');
    // order maintenance continues during a halt
    Bbo q = mgr.bbo(3);
    CHECK(q.has_bid && q.bid.price.raw() == 500'000);
    CHECK(q.has_ask && q.ask.price.raw() == 510'000);

    parse(trading, mgr);
    CHECK(mgr.trading_state(3) == 'T');

    CHECK(prints.size() == 5);
    CHECK(prints[0].source == 'E');
    CHECK(prints[0].price.raw() == 500'000);
    CHECK(prints[0].shares == 30);
    CHECK(prints[0].side == 'B');
    CHECK(prints[0].match == 71);
    CHECK(prints[0].locate == 3);
    CHECK(prints[0].timestamp == 6);

    CHECK(prints[1].source == 'C');
    CHECK(prints[1].price.raw() == 495'000);
    CHECK(prints[1].shares == 20);
    CHECK(prints[1].side == 'B');

    CHECK(prints[2].source == 'P');
    CHECK(prints[2].price.raw() == 505'000);
    CHECK(prints[2].shares == 75);
    CHECK(prints[2].side == 'B');

    CHECK(prints[3].source == 'Q');
    CHECK(prints[3].shares == 120'000);
    CHECK(prints[3].price.raw() == 502'000);
    CHECK(prints[3].match == 75);
    CHECK(prints[3].side == ' ');

    CHECK(prints[4].source == 'B');
    CHECK(prints[4].match == 74);
    CHECK(prints[4].shares == 0);

    // the executes above also mutated the book: 100 - 30 - 20 - 10 = 40 left
    q = mgr.bbo(3);
    CHECK(q.has_bid && q.bid.shares == 40);
}

void mutation_robustness() {
    std::vector<std::byte> pristine;
    synth::Generator g(7, 5);
    g.preamble(pristine);
    for (int i = 0; i < 2000; ++i) g.step(pristine);
    g.finish(pristine);

    synth::Rng rng{99};
    for (int round = 0; round < 200; ++round) {
        std::vector<std::byte> buf = pristine;
        for (int hits = 0; hits < 40; ++hits)
            buf[rng.below(static_cast<std::uint32_t>(buf.size()))] =
                static_cast<std::byte>(rng.below(256));
        const std::size_t cut = rng.below(static_cast<std::uint32_t>(buf.size()) + 1);
        BookManager<> mgr;
        const auto r = parse(std::span(buf.data(), cut), mgr);
        CHECK(r.consumed <= cut);
    }
}

void partial_handler_compiles() {
    std::vector<std::byte> buf;
    enc::add_order(buf, 7, 1, 5001, 'B', 10, "MSFT", 4'210'000);
    enc::order_delete(buf, 7, 2, 5001);

    struct AddsOnly {
        int adds = 0;
        void on_add(const AddOrder&) { ++adds; }
    } h;
    auto r = parse(buf, h);
    CHECK(r.messages == 2);
    CHECK(h.adds == 1);
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
    decode_all_types();
    manager_trading_state_and_prints();
    mutation_robustness();
    partial_handler_compiles();
    RUN_END();
}
