#pragma once
#include <cstddef>

namespace geryon {

enum class PageProtection {
    None,
    ReadWrite,
    ReadOnly
};

class MemoryRegion {
public:
    // Allocates memory of the given size. Size is automatically rounded up to a multiple of system_page_size().
    explicit MemoryRegion(std::size_t size);
    ~MemoryRegion();

    // Prevent copy and move to ensure stable pointer
    MemoryRegion(const MemoryRegion&) = delete;
    MemoryRegion& operator=(const MemoryRegion&) = delete;

    void* base_address() const noexcept { return base_address_; }
    std::size_t size() const noexcept { return size_; }

    // Change protection for a specific sub-region.
    // Address must be page-aligned and length must be a multiple of page size.
    void set_protection(void* address, std::size_t length, PageProtection prot);

    // Get the OS page size
    static std::size_t system_page_size() noexcept;

private:
    void* base_address_{nullptr};
    std::size_t size_{0};
};

} // namespace geryon
