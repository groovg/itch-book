#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "encode.hpp"

namespace synth {

struct Rng {
    std::uint64_t s;

    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }

    std::uint32_t below(std::uint32_t n) { return static_cast<std::uint32_t>(next() % n); }
};

struct LiveOrder {
    std::uint64_t ref;
    std::uint16_t locate;
    bool buy;
    std::uint32_t price;
    std::uint32_t qty;
};

class Generator {
  public:
    Generator(std::uint64_t seed, std::uint16_t symbols)
        : rng_{seed ? seed : 1}, nsym_(symbols), mids_(symbols + 1u, 0) {}

    void preamble(std::vector<std::byte>& out) {
        enc::system_event(out, ++ts_, 'O');
        enc::system_event(out, ++ts_, 'S');
        for (std::uint16_t i = 1; i <= nsym_; ++i) {
            enc::stock_directory(out, i, ++ts_, "SYM" + std::to_string(i), 100);
            mids_[i] = 200'000 + rng_.below(200) * 10'000;
        }
        enc::system_event(out, ++ts_, 'Q');
    }

    void step(std::vector<std::byte>& out) {
        const std::uint32_t roll = rng_.below(100);
        if (roll < 35 || live_.empty())
            add(out);
        else if (roll < 60)
            del(out);
        else if (roll < 73)
            exec(out);
        else if (roll < 83)
            cancel(out);
        else if (roll < 94)
            replace(out);
        else
            noise(out);
    }

    void finish(std::vector<std::byte>& out) {
        enc::system_event(out, ++ts_, 'M');
        enc::system_event(out, ++ts_, 'E');
        enc::system_event(out, ++ts_, 'C');
        enc::end_of_session(out);
    }

  private:
    void add(std::vector<std::byte>& out) {
        const std::uint16_t locate = static_cast<std::uint16_t>(1 + rng_.below(nsym_));
        const bool buy = rng_.below(2) == 0;
        const std::uint32_t price = jitter(mids_[locate]);
        const std::uint32_t qty = 1 + rng_.below(2000);
        next_ref_ += 1 + (rng_.below(8) == 0 ? rng_.below(5000) : 0);
        const std::uint64_t ref = next_ref_;
        if (rng_.below(10) == 0)
            enc::add_order_mpid(out, locate, ++ts_, ref, buy ? 'B' : 'S', qty,
                                "SYM" + std::to_string(locate), price, "MPID");
        else
            enc::add_order(out, locate, ++ts_, ref, buy ? 'B' : 'S', qty,
                           "SYM" + std::to_string(locate), price);
        live_.push_back({ref, locate, buy, price, qty});
        if (rng_.below(16) == 0)
            mids_[locate] = jitter(mids_[locate]);
    }

    void del(std::vector<std::byte>& out) {
        const std::uint32_t i = rng_.below(static_cast<std::uint32_t>(live_.size()));
        enc::order_delete(out, live_[i].locate, ++ts_, live_[i].ref);
        bury(i);
    }

    void exec(std::vector<std::byte>& out) {
        const std::uint32_t i = rng_.below(static_cast<std::uint32_t>(live_.size()));
        LiveOrder& o = live_[i];
        std::uint32_t shares;
        if (rng_.below(12) == 0)
            shares = o.qty + 1 + rng_.below(100);
        else if (rng_.below(3) == 0)
            shares = o.qty;
        else
            shares = 1 + rng_.below(o.qty);
        if (rng_.below(5) == 0)
            enc::order_executed_price(out, o.locate, ++ts_, o.ref, shares, ++match_,
                                      rng_.below(2) == 0, jitter(o.price));
        else
            enc::order_executed(out, o.locate, ++ts_, o.ref, shares, ++match_);
        settle(i, shares);
    }

    void cancel(std::vector<std::byte>& out) {
        const std::uint32_t i = rng_.below(static_cast<std::uint32_t>(live_.size()));
        LiveOrder& o = live_[i];
        const std::uint32_t shares =
            rng_.below(4) == 0 ? o.qty : 1 + rng_.below(o.qty);
        enc::order_cancel(out, o.locate, ++ts_, o.ref, shares);
        settle(i, shares);
    }

    void replace(std::vector<std::byte>& out) {
        const std::uint32_t i = rng_.below(static_cast<std::uint32_t>(live_.size()));
        LiveOrder o = live_[i];
        next_ref_ += 1 + rng_.below(50);
        const std::uint64_t new_ref = next_ref_;
        const std::uint32_t price = jitter(o.price);
        const std::uint32_t qty = 1 + rng_.below(3000);
        enc::order_replace(out, o.locate, ++ts_, o.ref, new_ref, qty, price);
        bury(i);
        live_.push_back({new_ref, o.locate, o.buy, price, qty});
    }

    void noise(std::vector<std::byte>& out) {
        switch (rng_.below(4)) {
            case 0:
                enc::trade(out, static_cast<std::uint16_t>(1 + rng_.below(nsym_)), ++ts_,
                           1 + rng_.below(500), "SYMX", 300'000, ++match_);
                break;
            case 1: {
                enc::Builder b(out);
                b.ch('Y')
                    .u16(static_cast<std::uint16_t>(1 + rng_.below(nsym_)))
                    .u16(0)
                    .u48(++ts_)
                    .str("SYMX", 8)
                    .ch('0');
                b.done();
                break;
            }
            case 2:
                if (!graveyard_.empty()) {
                    const auto& g = graveyard_[rng_.below(
                        static_cast<std::uint32_t>(graveyard_.size()))];
                    if (rng_.below(2) == 0)
                        enc::order_delete(out, g.locate, ++ts_, g.ref);
                    else
                        enc::order_executed(out, g.locate, ++ts_, g.ref, 10, ++match_);
                } else {
                    enc::order_delete(out, 1, ++ts_, 3);
                }
                break;
            case 3: {
                enc::Builder b(out);
                b.ch('x').u16(0).u16(0).u48(++ts_).u64(rng_.next());
                b.done();
                break;
            }
        }
    }

    std::uint32_t jitter(std::uint32_t around) {
        const std::uint32_t off = rng_.below(60) * 100;
        std::uint32_t p = rng_.below(2) == 0 ? around + off : around - off;
        if (p < 100 || p > around + 100'000) p = around;
        return p;
    }

    void settle(std::uint32_t i, std::uint32_t shares) {
        if (shares >= live_[i].qty)
            bury(i);
        else
            live_[i].qty -= shares;
    }

    void bury(std::uint32_t i) {
        if (graveyard_.size() < 64)
            graveyard_.push_back(live_[i]);
        live_[i] = live_.back();
        live_.pop_back();
    }

    Rng rng_;
    std::uint16_t nsym_;
    std::uint64_t ts_ = 34'200'000'000'000ull;
    std::uint64_t next_ref_ = 999;
    std::uint64_t match_ = 0;
    std::vector<std::uint32_t> mids_;
    std::vector<LiveOrder> live_;
    std::vector<LiveOrder> graveyard_;
};

}  // namespace synth
