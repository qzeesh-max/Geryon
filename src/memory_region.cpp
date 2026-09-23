#include "geryon/memory_region.hpp"
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <atomic>
#endif

namespace geryon {

std::size_t MemoryRegion::system_page_size() noexcept {
#ifdef _WIN32
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    return sysInfo.dwPageSize;
#else
    return static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#endif
}

MemoryRegion::MemoryRegion(std::size_t size) {
    std::size_t page_size = system_page_size();
    size_ = (size + page_size - 1) / page_size * page_size;

#ifdef _WIN32
    mapping_handle_ = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, size_, NULL);
    if (!mapping_handle_) {
        throw std::system_error(GetLastError(), std::system_category(), "CreateFileMappingA failed");
    }

    base_address_ = MapViewOfFile(mapping_handle_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size_);
    if (!base_address_) {
        CloseHandle(mapping_handle_);
        throw std::system_error(GetLastError(), std::system_category(), "MapViewOfFile failed");
    }

    DWORD old_prot;
    if (!VirtualProtect(base_address_, size_, PAGE_NOACCESS, &old_prot)) {
        UnmapViewOfFile(base_address_);
        CloseHandle(mapping_handle_);
        throw std::system_error(GetLastError(), std::system_category(), "VirtualProtect failed on base_address");
    }

    io_address_ = MapViewOfFile(mapping_handle_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size_);
    if (!io_address_) {
        UnmapViewOfFile(base_address_);
        CloseHandle(mapping_handle_);
        throw std::system_error(GetLastError(), std::system_category(), "MapViewOfFile for io_address failed");
    }
#else
    static std::atomic<int> region_counter{0};
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "/geryon_shm_%d_%d", getpid(), region_counter.fetch_add(1));
    
    int fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "shm_open failed");
    }
    
    // Immediately unlink so it cleans up when closed
    shm_unlink(shm_name);

    if (ftruncate(fd, size_) != 0) {
        close(fd);
        throw std::system_error(errno, std::generic_category(), "ftruncate failed");
    }

    base_address_ = mmap(nullptr, size_, PROT_NONE, MAP_SHARED, fd, 0);
    if (base_address_ == MAP_FAILED) {
        close(fd);
        throw std::system_error(errno, std::generic_category(), "mmap base_address failed");
    }

    io_address_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (io_address_ == MAP_FAILED) {
        munmap(base_address_, size_);
        close(fd);
        throw std::system_error(errno, std::generic_category(), "mmap io_address failed");
    }

    close(fd);
#endif
}

MemoryRegion::~MemoryRegion() {
#ifdef _WIN32
    if (base_address_) UnmapViewOfFile(base_address_);
    if (io_address_) UnmapViewOfFile(io_address_);
    if (mapping_handle_) CloseHandle(mapping_handle_);
#else
    if (base_address_) munmap(base_address_, size_);
    if (io_address_) munmap(io_address_, size_);
#endif
}

void MemoryRegion::set_protection(void* address, std::size_t length, PageProtection prot) {
#ifdef _WIN32
    DWORD win_prot = PAGE_NOACCESS;
    switch (prot) {
        case PageProtection::None: win_prot = PAGE_NOACCESS; break;
        case PageProtection::ReadWrite: win_prot = PAGE_READWRITE; break;
        case PageProtection::ReadOnly: win_prot = PAGE_READONLY; break;
    }
    DWORD old_prot;
    if (!VirtualProtect(address, length, win_prot, &old_prot)) {
        throw std::system_error(GetLastError(), std::system_category(), "VirtualProtect failed");
    }
#else
    int posix_prot = PROT_NONE;
    switch (prot) {
        case PageProtection::None: posix_prot = PROT_NONE; break;
        case PageProtection::ReadWrite: posix_prot = PROT_READ | PROT_WRITE; break;
        case PageProtection::ReadOnly: posix_prot = PROT_READ; break;
    }
    if (mprotect(address, length, posix_prot) != 0) {
        throw std::system_error(errno, std::generic_category(), "mprotect failed");
    }
#endif
}

} // namespace geryon
