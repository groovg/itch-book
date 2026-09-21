# Contributing

## Build and test

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

The C++ core is header-only and lives in `include/`; tests are in `test/`, benchmarks in
`bench/` and fuzz targets in `fuzz/`. The latency bench needs `-DITCH_BENCH_LATENCY=ON`
and is x86 only.

GCC and Clang are supported. MSVC is not: the price type needs `__int128`, so on Windows
build with clang-cl.

## Pull requests

Keep the hot path free of virtual calls and unaligned accesses — decoding goes through
`memcpy` plus `std::byteswap` so the parser stays clean under UBSan. If you change parsing
or book application, run `ctest` and the fuzz targets before opening a PR.
