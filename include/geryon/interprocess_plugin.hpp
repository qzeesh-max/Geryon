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
#include "geryon/memory_region.hpp"
#include <boost/interprocess/managed_external_buffer.hpp>

namespace geryon {

class InterprocessRegion {
public:
    // Create a new managed buffer inside the region (typically on Primary)
    static boost::interprocess::managed_external_buffer create(MemoryRegion& region) {
        return boost::interprocess::managed_external_buffer(
            boost::interprocess::create_only,
            region.base_address(),
            region.size());
    }

    // Open an existing managed buffer (typically on Replica)
    static boost::interprocess::managed_external_buffer open(MemoryRegion& region) {
        return boost::interprocess::managed_external_buffer(
            boost::interprocess::open_only,
            region.base_address(),
            region.size());
    }
};

} // namespace geryon
