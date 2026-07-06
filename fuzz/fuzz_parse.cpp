#include <itch/book_manager.hpp>
#include <itch/parser.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(data), size};
    itch::BookManager<> mgr;
    itch::parse(bytes, mgr);
    return 0;
}
