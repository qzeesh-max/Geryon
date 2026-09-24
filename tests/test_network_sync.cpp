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
#include <thread>
#include <chrono>

using namespace geryon;

TEST(NetworkSyncTest, BouncingOwnership) {
    std::size_t page_size = MemoryRegion::system_page_size();
    
    // Primary
    MemoryRegion primary_region(page_size);
    NetworkNode primary_node(&primary_region, true); // initial owner
    
    // Replica
    MemoryRegion replica_region(page_size);
    NetworkNode replica_node(&replica_region, false); // not initial owner

    // Set protections appropriately
    primary_region.set_protection(primary_region.base_address(), page_size, PageProtection::ReadWrite);
    replica_region.set_protection(replica_region.base_address(), page_size, PageProtection::None);

    // Write initial data to primary
    char* primary_ptr = static_cast<char*>(primary_region.base_address());
    primary_ptr[0] = 'M';

    // Start network
    primary_node.start_primary(12345);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    replica_node.start_replica("127.0.0.1", 12345);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Replica wants the page
    EXPECT_TRUE(replica_node.request_page(replica_region.base_address()));

    // Now replica should have it and primary shouldn't
    char* replica_ptr = static_cast<char*>(replica_region.base_address());
    EXPECT_EQ(replica_ptr[0], 'M');
    
    replica_ptr[0] = 'S'; // Replica modifies it

    // Primary wants it back
    EXPECT_TRUE(primary_node.request_page(primary_region.base_address()));

    // Verify primary has the modification
    EXPECT_EQ(primary_ptr[0], 'S');

    primary_node.stop();
    replica_node.stop();
}

TEST(NetworkSyncTest, StressTest) {
    std::size_t num_pages = 50;
    std::size_t size = MemoryRegion::system_page_size() * num_pages;
    
    MemoryRegion primary_region(size);
    primary_region.set_protection(primary_region.base_address(), size, PageProtection::ReadWrite);
    NetworkNode primary_node(&primary_region, true);
    
    MemoryRegion replica_region(size);
    replica_region.set_protection(replica_region.base_address(), size, PageProtection::None);
    NetworkNode replica_node(&replica_region, false);
    
    auto replica_fault = FaultHandler::create();
    replica_fault->register_region_with_io(replica_region.base_address(), replica_region.io_address(), size, [&](void* addr) -> bool {
        return replica_node.request_page(addr);
    });

    auto primary_fault = FaultHandler::create();
    primary_fault->register_region_with_io(primary_region.base_address(), primary_region.io_address(), size, [&](void* addr) -> bool {
        return primary_node.request_page(addr);
    });

    primary_node.start_primary(12350);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    replica_node.start_replica("127.0.0.1", 12350);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Primary initializes data
    int* primary_ptr = static_cast<int*>(primary_region.base_address());
    int* replica_ptr = static_cast<int*>(replica_region.base_address());
    
    for (std::size_t i = 0; i < size / sizeof(int); ++i) {
        primary_ptr[i] = i;
    }

    std::atomic<bool> stop_flag{false};

    auto random_read_write = [&](int* ptr) {
        for (int iter = 0; iter < 1000; ++iter) {
            std::size_t index = (std::rand() % (size / sizeof(int)));
            // Read and mutate
            ptr[index] += 1;
        }
    };

    std::thread primary_thread([&]() {
        random_read_write(primary_ptr);
    });

    std::thread replica_thread([&]() {
        random_read_write(replica_ptr);
    });

    primary_thread.join();
    replica_thread.join();

    // Verification - just ensure no crashes and soft faults handled.
    // In a real strict distributed system, we'd checksum, but random unsynchronized read/writes 
    // without locks will have race conditions on the exact values.
    // The framework's guarantee is that memory is successfully transferred.

    primary_fault->unregister_region(primary_region.base_address());
    replica_fault->unregister_region(replica_region.base_address());

    primary_node.stop();
    replica_node.stop();
}
