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

#include <gtest/gtest.h>
#include "geryon/memory_region.hpp"
#include "geryon/network_node.hpp"
#include "geryon/fault_handler.hpp"
#include "geryon/interprocess_plugin.hpp"
#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/containers/vector.hpp>
#include <thread>
#include <chrono>

using namespace geryon;
namespace bi = boost::interprocess;

TEST(InterprocessTest, SharedVector) {
    std::size_t size = MemoryRegion::system_page_size() * 10;
    
    // Primary setup
    MemoryRegion primary_region(size);
    primary_region.set_protection(primary_region.base_address(), size, PageProtection::ReadWrite);
    NetworkNode primary_node(&primary_region, true);
    
    // Replica setup
    MemoryRegion replica_region(size);
    replica_region.set_protection(replica_region.base_address(), size, PageProtection::None);
    NetworkNode replica_node(&replica_region, false);
    
    // Fault Handler on Replica
    auto fault_handler = FaultHandler::create();
    fault_handler->register_region(replica_region.base_address(), size, [&](void* addr) -> bool {
        return replica_node.request_page(addr);
    });

    primary_node.start_primary(12346);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    replica_node.start_replica("127.0.0.1", 12346);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Primary creates vector
    auto primary_buffer = InterprocessRegion::create(primary_region);
    typedef bi::allocator<int, bi::managed_external_buffer::segment_manager> IntAllocator;
    typedef bi::vector<int, IntAllocator> MyVector;

    const IntAllocator alloc_inst(primary_buffer.get_segment_manager());
    MyVector* my_vec = primary_buffer.construct<MyVector>("MyVector")(alloc_inst);
    my_vec->push_back(42);
    my_vec->push_back(100);

    // Replica accesses the vector.
    // The call to `open` will touch the first page, causing a fault and fetching it.
    auto replica_buffer = InterprocessRegion::open(replica_region);
    
    // `find` will read metadata and the vector object, potentially faulting and fetching more pages.
    std::pair<MyVector*, bi::managed_external_buffer::size_type> res = replica_buffer.find<MyVector>("MyVector");
    
    ASSERT_NE(res.first, nullptr);
    EXPECT_EQ(res.first->size(), 2);
    EXPECT_EQ((*res.first)[0], 42);
    EXPECT_EQ((*res.first)[1], 100);

    // Replica modifies
    res.first->push_back(256);

    // Fault Handler on Primary
    // Since primary gave up some pages, it needs a fault handler too!
    auto primary_fault_handler = FaultHandler::create();
    primary_fault_handler->register_region(primary_region.base_address(), size, [&](void* addr) -> bool {
        return primary_node.request_page(addr);
    });

    // Primary reads the modification
    EXPECT_EQ(my_vec->size(), 3);
    EXPECT_EQ((*my_vec)[2], 256);

    primary_fault_handler->unregister_region(primary_region.base_address());
    fault_handler->unregister_region(replica_region.base_address());

    primary_node.stop();
    replica_node.stop();
}
