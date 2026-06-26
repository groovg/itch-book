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

inline ParseResult scan(std::span<const std::byte> data) {
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
        const char type = wire::ch(p + pos + 2);
        const std::size_t want = wire_length(type);
        if (want == len) ++r.messages;
        else if (want == 0) ++r.unknown;
        else ++r.malformed;
        pos += 2 + len;
    }
    r.consumed = pos;
    return r;
}

}  // namespace itch
