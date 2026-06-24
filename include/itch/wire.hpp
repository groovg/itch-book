#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace itch::wire {

inline std::uint16_t u16(const std::byte* p) {
    std::uint16_t v;
    std::memcpy(&v, p, sizeof v);
    if constexpr (std::endian::native == std::endian::little) v = std::byteswap(v);
    return v;
}

inline std::uint32_t u32(const std::byte* p) {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    if constexpr (std::endian::native == std::endian::little) v = std::byteswap(v);
    return v;
}

inline std::uint64_t u64(const std::byte* p) {
    std::uint64_t v;
    std::memcpy(&v, p, sizeof v);
    if constexpr (std::endian::native == std::endian::little) v = std::byteswap(v);
    return v;
}

inline std::uint64_t u48(const std::byte* p) {
    std::uint64_t v = 0;
    std::memcpy(&v, p, 6);
    if constexpr (std::endian::native == std::endian::little) v = std::byteswap(v);
    return v >> 16;
}

template <std::size_t N>
struct Alpha {
    std::array<char, N> raw;

    std::string_view view() const {
        std::string_view s{raw.data(), N};
        while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
        return s;
    }

    bool operator==(const Alpha&) const = default;
};

template <std::size_t N>
inline Alpha<N> alpha(const std::byte* p) {
    Alpha<N> a;
    std::memcpy(a.raw.data(), p, N);
    return a;
}

inline char ch(const std::byte* p) { return static_cast<char>(*p); }

}  // namespace itch::wire
