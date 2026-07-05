#include "synthetic.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3 || argc > 5) {
        std::fprintf(stderr, "usage: gen-synthetic <out-file> <messages> [symbols=50] [seed=1]\n");
        return 2;
    }
    const std::uint64_t total = std::strtoull(argv[2], nullptr, 10);
    const std::uint16_t symbols =
        argc > 3 ? static_cast<std::uint16_t>(std::strtoul(argv[3], nullptr, 10)) : 50;
    const std::uint64_t seed = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 1;

    std::ofstream out(argv[1], std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }

    synth::Generator g(seed, symbols);
    std::vector<std::byte> buf;
    g.preamble(buf);
    for (std::uint64_t i = 0; i < total; ++i) {
        g.step(buf);
        if (buf.size() > (1u << 20)) {
            out.write(reinterpret_cast<const char*>(buf.data()),
                      static_cast<std::streamsize>(buf.size()));
            buf.clear();
        }
    }
    g.finish(buf);
    out.write(reinterpret_cast<const char*>(buf.data()),
              static_cast<std::streamsize>(buf.size()));
    std::fprintf(stderr, "wrote %s: ~%llu messages, %u symbols, seed %llu\n", argv[1],
                 static_cast<unsigned long long>(total), symbols,
                 static_cast<unsigned long long>(seed));
    return 0;
}
