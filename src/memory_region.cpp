#include "geryon/memory_region.hpp"
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
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
    base_address_ = VirtualAlloc(NULL, size_, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    if (!base_address_) {
        throw std::system_error(GetLastError(), std::system_category(), "VirtualAlloc failed");
    }
#else
    base_address_ = mmap(nullptr, size_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base_address_ == MAP_FAILED) {
        throw std::system_error(errno, std::generic_category(), "mmap failed");
    }
#endif
}

MemoryRegion::~MemoryRegion() {
    if (base_address_) {
#ifdef _WIN32
        VirtualFree(base_address_, 0, MEM_RELEASE);
#else
        munmap(base_address_, size_);
#endif
    }
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
