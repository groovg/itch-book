#pragma once

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace itch {

class MappedFile {
  public:
    explicit MappedFile(const std::string& path) {
#if defined(_WIN32)
        HANDLE file = ::CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("open failed: " + path);
        LARGE_INTEGER sz{};
        if (!::GetFileSizeEx(file, &sz)) {
            ::CloseHandle(file);
            throw std::runtime_error("stat failed: " + path);
        }
        size_ = static_cast<std::size_t>(sz.QuadPart);
        if (size_ != 0) {
            HANDLE mapping = ::CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
            ::CloseHandle(file);
            if (!mapping) throw std::runtime_error("mapping failed: " + path);
            data_ = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
            ::CloseHandle(mapping);
            if (!data_) throw std::runtime_error("map view failed: " + path);
        } else {
            ::CloseHandle(file);
        }
#else
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) throw std::runtime_error("open failed: " + path);
        struct stat st{};
        if (::fstat(fd, &st) != 0) {
            ::close(fd);
            throw std::runtime_error("stat failed: " + path);
        }
        size_ = static_cast<std::size_t>(st.st_size);
        if (size_ != 0) {
            void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
            ::close(fd);
            if (p == MAP_FAILED) throw std::runtime_error("mmap failed: " + path);
            data_ = p;
#if defined(POSIX_MADV_SEQUENTIAL)
            ::posix_madvise(data_, size_, POSIX_MADV_SEQUENTIAL);
#endif
        } else {
            ::close(fd);
        }
#endif
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    ~MappedFile() {
        if (data_ == nullptr) return;
#if defined(_WIN32)
        ::UnmapViewOfFile(data_);
#else
        ::munmap(data_, size_);
#endif
    }

    std::span<const std::byte> bytes() const {
        return {static_cast<const std::byte*>(data_), size_};
    }

    std::size_t size() const { return size_; }

  private:
    void* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace itch
