#pragma once

#include <cstddef>

namespace itch {

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

}  // namespace itch
