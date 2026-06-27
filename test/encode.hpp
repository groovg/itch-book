#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace enc {

class Builder {
  public:
    explicit Builder(std::vector<std::byte>& out) : out_(out), start_(out.size() + 2) {
        out_.push_back(std::byte{0});
        out_.push_back(std::byte{0});
    }

    Builder& ch(char c) {
        push(c);
        return *this;
    }

    Builder& u16(std::uint16_t v) {
        push(v >> 8);
        push(v);
        return *this;
    }

    Builder& u32(std::uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) push(v >> s);
        return *this;
    }

    Builder& u48(std::uint64_t v) {
        for (int s = 40; s >= 0; s -= 8) push(v >> s);
        return *this;
    }

    Builder& u64(std::uint64_t v) {
        for (int s = 56; s >= 0; s -= 8) push(v >> s);
        return *this;
    }

    Builder& str(std::string_view s, std::size_t width) {
        for (std::size_t i = 0; i < width; ++i) push(i < s.size() ? s[i] : ' ');
        return *this;
    }

    void done() {
        const std::size_t len = out_.size() - start_;
        out_[start_ - 2] = static_cast<std::byte>(len >> 8);
        out_[start_ - 1] = static_cast<std::byte>(len & 0xff);
    }

  private:
    template <typename T>
    void push(T v) {
        out_.push_back(static_cast<std::byte>(static_cast<unsigned char>(v)));
    }

    std::vector<std::byte>& out_;
    std::size_t start_;
};

inline Builder msg(std::vector<std::byte>& out, char type, std::uint16_t locate,
                   std::uint64_t ts) {
    Builder b(out);
    b.ch(type).u16(locate).u16(0).u48(ts);
    return b;
}

inline void system_event(std::vector<std::byte>& out, std::uint64_t ts, char event) {
    msg(out, 'S', 0, ts).ch(event).done();
}

inline void stock_directory(std::vector<std::byte>& out, std::uint16_t locate,
                            std::uint64_t ts, std::string_view stock,
                            std::uint32_t round_lot) {
    msg(out, 'R', locate, ts)
        .str(stock, 8)
        .ch('Q')
        .ch(' ')
        .u32(round_lot)
        .ch('N')
        .ch('C')
        .str("", 2)
        .ch('P')
        .ch('N')
        .ch('N')
        .ch('1')
        .ch('N')
        .u32(0)
        .ch('N')
        .done();
}

inline void add_order(std::vector<std::byte>& out, std::uint16_t locate, std::uint64_t ts,
                      std::uint64_t ref, char side, std::uint32_t shares,
                      std::string_view stock, std::uint32_t price) {
    msg(out, 'A', locate, ts).u64(ref).ch(side).u32(shares).str(stock, 8).u32(price).done();
}

inline void add_order_mpid(std::vector<std::byte>& out, std::uint16_t locate,
                           std::uint64_t ts, std::uint64_t ref, char side,
                           std::uint32_t shares, std::string_view stock,
                           std::uint32_t price, std::string_view mpid) {
    msg(out, 'F', locate, ts)
        .u64(ref)
        .ch(side)
        .u32(shares)
        .str(stock, 8)
        .u32(price)
        .str(mpid, 4)
        .done();
}

inline void order_executed(std::vector<std::byte>& out, std::uint16_t locate,
                           std::uint64_t ts, std::uint64_t ref, std::uint32_t shares,
                           std::uint64_t match) {
    msg(out, 'E', locate, ts).u64(ref).u32(shares).u64(match).done();
}

inline void order_executed_price(std::vector<std::byte>& out, std::uint16_t locate,
                                 std::uint64_t ts, std::uint64_t ref, std::uint32_t shares,
                                 std::uint64_t match, bool printable, std::uint32_t price) {
    msg(out, 'C', locate, ts)
        .u64(ref)
        .u32(shares)
        .u64(match)
        .ch(printable ? 'Y' : 'N')
        .u32(price)
        .done();
}

inline void order_cancel(std::vector<std::byte>& out, std::uint16_t locate, std::uint64_t ts,
                         std::uint64_t ref, std::uint32_t shares) {
    msg(out, 'X', locate, ts).u64(ref).u32(shares).done();
}

inline void order_delete(std::vector<std::byte>& out, std::uint16_t locate, std::uint64_t ts,
                         std::uint64_t ref) {
    msg(out, 'D', locate, ts).u64(ref).done();
}

inline void order_replace(std::vector<std::byte>& out, std::uint16_t locate,
                          std::uint64_t ts, std::uint64_t old_ref, std::uint64_t new_ref,
                          std::uint32_t shares, std::uint32_t price) {
    msg(out, 'U', locate, ts).u64(old_ref).u64(new_ref).u32(shares).u32(price).done();
}

inline void trade(std::vector<std::byte>& out, std::uint16_t locate, std::uint64_t ts,
                  std::uint32_t shares, std::string_view stock, std::uint32_t price,
                  std::uint64_t match) {
    msg(out, 'P', locate, ts).u64(0).ch('B').u32(shares).str(stock, 8).u32(price).u64(match).done();
}

inline void end_of_session(std::vector<std::byte>& out) {
    out.push_back(std::byte{0});
    out.push_back(std::byte{0});
}

}  // namespace enc
