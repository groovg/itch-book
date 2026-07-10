#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include "parser.hpp"
#include "wire.hpp"

namespace itch {

// Reassembles length-prefixed messages that arrive split across arbitrary chunk
// boundaries (socket reads, packet payloads). Whole frames inside a chunk are parsed
// in place; only a partial tail is ever copied, so the buffer is bounded by one frame.
template <typename Handler>
class StreamParser {
  public:
    explicit StreamParser(Handler& h) : h_(h) { pending_.reserve(kMaxFrame); }

    const ParseResult& feed(std::span<const std::byte> chunk) {
        const std::byte* p = chunk.data();
        std::size_t n = chunk.size();
        if (r_.end_of_session) return r_;
        while (n > 0 && !pending_.empty()) {
            const std::size_t need = pending_need();
            const std::size_t take = need < n ? need : n;
            append(p, take);
            p += take;
            n -= take;
            if (pending_need() == 0) {
                accumulate(parse(pending_, h_));
                pending_.clear();
                if (r_.end_of_session) return r_;
            }
        }
        const ParseResult bulk = parse({p, n}, h_);
        accumulate(bulk);
        if (!r_.end_of_session && bulk.consumed < n) append(p + bulk.consumed, n - bulk.consumed);
        return r_;
    }

    const ParseResult& result() const { return r_; }
    std::size_t pending_bytes() const { return pending_.size(); }

  private:
    static constexpr std::size_t kMaxFrame = 2 + 0xffff;

    void append(const std::byte* p, std::size_t n) {
        const std::size_t old = pending_.size();
        pending_.resize(old + n);
        std::memcpy(pending_.data() + old, p, n);
    }

    std::size_t pending_need() const {
        if (pending_.size() < 2) return 2 - pending_.size();
        return 2 + wire::u16(pending_.data()) - pending_.size();
    }

    void accumulate(const ParseResult& r) {
        r_.consumed += r.consumed;
        r_.messages += r.messages;
        r_.unknown += r.unknown;
        r_.malformed += r.malformed;
        if (r.end_of_session) r_.end_of_session = true;
    }

    Handler& h_;
    std::vector<std::byte> pending_;
    ParseResult r_;
};

}  // namespace itch
