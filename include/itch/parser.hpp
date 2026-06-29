#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "messages.hpp"
#include "wire.hpp"

namespace itch {

struct ParseResult {
    std::size_t consumed = 0;
    std::uint64_t messages = 0;
    std::uint64_t unknown = 0;
    std::uint64_t malformed = 0;
    bool end_of_session = false;
};

namespace detail {

template <typename H>
inline void dispatch(char type, const std::byte* m, H& h) {
    switch (type) {
        case 'S':
            if constexpr (requires { h.on_system_event(SystemEvent::decode(m)); })
                h.on_system_event(SystemEvent::decode(m));
            break;
        case 'R':
            if constexpr (requires { h.on_stock_directory(StockDirectory::decode(m)); })
                h.on_stock_directory(StockDirectory::decode(m));
            break;
        case 'A':
            if constexpr (requires { h.on_add(AddOrder::decode(m, false)); })
                h.on_add(AddOrder::decode(m, false));
            break;
        case 'F':
            if constexpr (requires { h.on_add(AddOrder::decode(m, true)); })
                h.on_add(AddOrder::decode(m, true));
            break;
        case 'E':
            if constexpr (requires { h.on_execute(OrderExecuted::decode(m)); })
                h.on_execute(OrderExecuted::decode(m));
            break;
        case 'C':
            if constexpr (requires { h.on_execute_price(OrderExecutedPrice::decode(m)); })
                h.on_execute_price(OrderExecutedPrice::decode(m));
            break;
        case 'X':
            if constexpr (requires { h.on_cancel(OrderCancel::decode(m)); })
                h.on_cancel(OrderCancel::decode(m));
            break;
        case 'D':
            if constexpr (requires { h.on_delete(OrderDelete::decode(m)); })
                h.on_delete(OrderDelete::decode(m));
            break;
        case 'U':
            if constexpr (requires { h.on_replace(OrderReplace::decode(m)); })
                h.on_replace(OrderReplace::decode(m));
            break;
        case 'P':
            if constexpr (requires { h.on_trade(Trade::decode(m)); })
                h.on_trade(Trade::decode(m));
            break;
        default:
            if constexpr (requires { h.on_other(type); }) h.on_other(type);
            break;
    }
}

}  // namespace detail

template <typename Handler>
ParseResult parse(std::span<const std::byte> data, Handler&& h) {
    ParseResult r;
    const std::byte* p = data.data();
    const std::size_t n = data.size();
    std::size_t pos = 0;
    while (n - pos >= 2) {
        const std::size_t len = wire::u16(p + pos);
        if (len == 0) {
            r.consumed = pos + 2;
            r.end_of_session = true;
            return r;
        }
        if (len > n - pos - 2) break;
        const std::byte* m = p + pos + 2;
        const char type = wire::ch(m);
        const std::size_t want = wire_length(type);
        if (want == len) {
            detail::dispatch(type, m, h);
            ++r.messages;
        } else if (want == 0) {
            ++r.unknown;
        } else {
            ++r.malformed;
        }
        pos += 2 + len;
    }
    r.consumed = pos;
    return r;
}

struct NoopHandler {};

inline ParseResult scan(std::span<const std::byte> data) {
    NoopHandler h;
    return parse(data, h);
}

}  // namespace itch
