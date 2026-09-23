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
