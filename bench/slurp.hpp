#pragma once

#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace bench {

inline std::vector<std::byte> slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("cannot open " + path);
    const std::streamsize size = in.tellg();
    in.seekg(0);
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    constexpr std::streamsize kChunk = 1 << 28;
    for (std::streamsize off = 0; off < size;) {
        const std::streamsize want = size - off < kChunk ? size - off : kChunk;
        if (!in.read(reinterpret_cast<char*>(data.data()) + off, want))
            throw std::runtime_error("short read: " + path);
        off += want;
    }
    return data;
}

}  // namespace bench
