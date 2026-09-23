/*
 * Geryon - A Distributed Shared Memory Framework
 * Copyright (C) 2026 Zeeshan Qazi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once
#include <cstddef>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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
    void* io_address() const noexcept { return io_address_; }
    std::size_t size() const noexcept { return size_; }

    // Change protection for a specific sub-region.
    // Address must be page-aligned and length must be a multiple of page size.
    void set_protection(void* address, std::size_t length, PageProtection prot);

    // Get the OS page size
    static std::size_t system_page_size() noexcept;

private:
    void* base_address_{nullptr};
    void* io_address_{nullptr};
    std::size_t size_{0};
#ifdef _WIN32
    HANDLE mapping_handle_{NULL};
#endif
};

} // namespace geryon
