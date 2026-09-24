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
#include <functional>
#include <memory>

namespace geryon {

// A callback invoked when a page fault occurs in a registered memory region.
// It receives the faulting address.
// The callback must handle the fault (e.g. fetch data, update page protection to R/W)
// and return true to resume the faulting thread, or false to trigger a crash.
using PageFaultCallback = std::function<bool(void* fault_address)>;

class FaultHandler {
public:
    virtual ~FaultHandler() = default;

    virtual void register_region(void* base_address, std::size_t size, PageFaultCallback callback) = 0;
    virtual void unregister_region(void* base_address) = 0;

    // Extended registration providing the io_address mirror.
    // On Linux, this allows the handler to issue copies to the secondary mapping
    // with real page data. On other platforms, this
    // defaults to register_region() (io_address is unused).
    virtual void register_region_with_io(void* base_address, void* io_address,
                                         std::size_t size, PageFaultCallback callback) {
        (void)io_address;
        register_region(base_address, size, std::move(callback));
    }

    // Factory method to get the platform-specific fault handler instance
    static std::unique_ptr<FaultHandler> create();
};

} // namespace geryon
