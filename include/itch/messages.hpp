#pragma once

#include <cstddef>
#include <cstdint>

#include <fixed_decimal.hpp>

#include "wire.hpp"

namespace itch {

using Price = fixed_decimal::Fixed<4, fixed_decimal::PriceTag, std::int64_t>;

inline Price price4(const std::byte* p) {
    return Price::from_raw(static_cast<std::int64_t>(wire::u32(p)));
}

enum class MsgType : char {
    SystemEvent = 'S',
    StockDirectory = 'R',
    TradingAction = 'H',
    RegSho = 'Y',
    ParticipantPosition = 'L',
    MwcbDecline = 'V',
    MwcbStatus = 'W',
    IpoQuoting = 'K',
    LuldCollar = 'J',
    OperationalHalt = 'h',
    AddOrder = 'A',
    AddOrderMpid = 'F',
    OrderExecuted = 'E',
    OrderExecutedPrice = 'C',
    OrderCancel = 'X',
    OrderDelete = 'D',
    OrderReplace = 'U',
    Trade = 'P',
    CrossTrade = 'Q',
    BrokenTrade = 'B',
    Noii = 'I',
    Rpii = 'N',
    DirectListing = 'O',
};

constexpr std::size_t wire_length(char type) {
    switch (type) {
        case 'S': return 12;
        case 'R': return 39;
        case 'H': return 25;
        case 'Y': return 20;
        case 'L': return 26;
        case 'V': return 35;
        case 'W': return 12;
        case 'K': return 28;
        case 'J': return 35;
        case 'h': return 21;
        case 'A': return 36;
        case 'F': return 40;
        case 'E': return 31;
        case 'C': return 36;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        case 'P': return 44;
        case 'Q': return 40;
        case 'B': return 19;
        case 'I': return 50;
        case 'N': return 20;
        case 'O': return 48;
        default: return 0;
    }
}

enum class Side : char { Buy = 'B', Sell = 'S' };

struct Header {
    std::uint16_t locate;
    std::uint16_t tracking;
    std::uint64_t timestamp;
};

inline Header header(const std::byte* m) {
    return {wire::u16(m + 1), wire::u16(m + 3), wire::u48(m + 5)};
}

struct SystemEvent {
    Header hdr;
    char event;

    static SystemEvent decode(const std::byte* m) {
        return {header(m), wire::ch(m + 11)};
    }
};

struct StockDirectory {
    Header hdr;
    wire::Alpha<8> stock;
    char market_category;
    char financial_status;
    std::uint32_t round_lot_size;
    bool round_lots_only;
    char issue_classification;
    wire::Alpha<2> issue_subtype;
    char authenticity;
    char short_sale_threshold;
    char ipo_flag;
    char luld_tier;
    char etp_flag;
    std::uint32_t etp_leverage;
    char inverse;

    static StockDirectory decode(const std::byte* m) {
        return {header(m),
                wire::alpha<8>(m + 11),
                wire::ch(m + 19),
                wire::ch(m + 20),
                wire::u32(m + 21),
                wire::ch(m + 25) == 'Y',
                wire::ch(m + 26),
                wire::alpha<2>(m + 27),
                wire::ch(m + 29),
                wire::ch(m + 30),
                wire::ch(m + 31),
                wire::ch(m + 32),
                wire::ch(m + 33),
                wire::u32(m + 34),
                wire::ch(m + 38)};
    }
};

struct AddOrder {
    Header hdr;
    std::uint64_t ref;
    Side side;
    std::uint32_t shares;
    wire::Alpha<8> stock;
    Price price;
    wire::Alpha<4> mpid;
    bool attributed;

    static AddOrder decode(const std::byte* m, bool attributed) {
        return {header(m),
                wire::u64(m + 11),
                Side{wire::ch(m + 19)},
                wire::u32(m + 20),
                wire::alpha<8>(m + 24),
                price4(m + 32),
                attributed ? wire::alpha<4>(m + 36) : wire::Alpha<4>{{' ', ' ', ' ', ' '}},
                attributed};
    }
};

struct OrderExecuted {
    Header hdr;
    std::uint64_t ref;
    std::uint32_t shares;
    std::uint64_t match;

    static OrderExecuted decode(const std::byte* m) {
        return {header(m), wire::u64(m + 11), wire::u32(m + 19), wire::u64(m + 23)};
    }
};

struct OrderExecutedPrice {
    Header hdr;
    std::uint64_t ref;
    std::uint32_t shares;
    std::uint64_t match;
    bool printable;
    Price price;

    static OrderExecutedPrice decode(const std::byte* m) {
        return {header(m),          wire::u64(m + 11),       wire::u32(m + 19),
                wire::u64(m + 23),  wire::ch(m + 31) == 'Y', price4(m + 32)};
    }
};

struct OrderCancel {
    Header hdr;
    std::uint64_t ref;
    std::uint32_t shares;

    static OrderCancel decode(const std::byte* m) {
        return {header(m), wire::u64(m + 11), wire::u32(m + 19)};
    }
};

struct OrderDelete {
    Header hdr;
    std::uint64_t ref;

    static OrderDelete decode(const std::byte* m) {
        return {header(m), wire::u64(m + 11)};
    }
};

struct OrderReplace {
    Header hdr;
    std::uint64_t old_ref;
    std::uint64_t new_ref;
    std::uint32_t shares;
    Price price;

    static OrderReplace decode(const std::byte* m) {
        return {header(m), wire::u64(m + 11), wire::u64(m + 19), wire::u32(m + 27),
                price4(m + 31)};
    }
};

struct Trade {
    Header hdr;
    std::uint64_t ref;
    Side side;
    std::uint32_t shares;
    wire::Alpha<8> stock;
    Price price;
    std::uint64_t match;

    static Trade decode(const std::byte* m) {
        return {header(m),         wire::u64(m + 11), Side{wire::ch(m + 19)},
                wire::u32(m + 20), wire::alpha<8>(m + 24), price4(m + 32),
                wire::u64(m + 36)};
    }
};

}  // namespace itch
