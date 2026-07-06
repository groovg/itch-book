#pragma once

#include <cstddef>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace bench {

inline std::size_t peak_rss_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    ::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof pmc);
    return pmc.PeakWorkingSetSize;
#else
    struct rusage ru{};
    ::getrusage(RUSAGE_SELF, &ru);
    return static_cast<std::size_t>(ru.ru_maxrss) * 1024;
#endif
}

}  // namespace bench
