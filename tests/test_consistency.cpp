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

class ConsistencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        setvbuf(stdout, NULL, _IONBF, 0);
    }
};

TEST_F(ConsistencyTest, TriggerSynchronization) {
    std::size_t region_size = 1024 * 1024; // 1 MB
    MemoryRegion primary_region(region_size);
    MemoryRegion replica_region(region_size);
    
    NetworkNode primary_node(&primary_region, true);
    std::cerr << "Starting primary..." << std::endl;
    primary_node.start_primary(12347);
    std::cerr << "Primary started." << std::endl;
    
    NetworkNode replica_node(&replica_region, false);
    std::cerr << "Starting replica..." << std::endl;
    replica_node.start_replica("127.0.0.1", 12347);
    std::cerr << "Replica started." << std::endl;
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    std::cerr << "Simulating replica page request 1" << std::endl;
    char* replica_ptr = static_cast<char*>(replica_region.base_address());
    replica_node.request_page(replica_ptr + 4096);
    std::cerr << "Simulating replica page request 2" << std::endl;
    replica_node.request_page(replica_ptr + 8192);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::cerr << "Replica writing to pages" << std::endl;
    replica_ptr[4096] = 42;
    replica_ptr[8192] = 84;
    
    std::cerr << "Primary triggering synchronization" << std::endl;
    primary_node.trigger_synchronization();
    
    std::cerr << "Primary checking consistency" << std::endl;
    char* primary_ptr = static_cast<char*>(primary_region.base_address());
    EXPECT_EQ(primary_ptr[4096], 42);
    EXPECT_EQ(primary_ptr[8192], 84);
    
    std::cerr << "Stopping nodes" << std::endl;
    replica_node.stop();
    primary_node.stop();
    std::cerr << "Test complete" << std::endl;
}

TEST_F(ConsistencyTest, ReplicaStopFlushesData) {
    std::size_t region_size = 1024 * 1024;
    MemoryRegion primary_region(region_size);
    MemoryRegion replica_region(region_size);
    
    NetworkNode primary_node(&primary_region, true);
    primary_node.start_primary(12348);
    
    NetworkNode replica_node(&replica_region, false);
    replica_node.start_replica("127.0.0.1", 12348);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    char* replica_ptr = static_cast<char*>(replica_region.base_address());
    replica_node.request_page(replica_ptr);
    replica_ptr[0] = 99;
    
    // Replica stops, which should trigger a flush!
    replica_node.stop();
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Allow primary to process incoming PageData
    
    char* primary_ptr = static_cast<char*>(primary_region.base_address());
    EXPECT_EQ(primary_ptr[0], 99);
    
    primary_node.stop();
}

TEST_F(ConsistencyTest, PrimaryStopInitiatesHandshake) {
    std::size_t region_size = 1024 * 1024;
    MemoryRegion primary_region(region_size);
    MemoryRegion replica_region(region_size);
    
    NetworkNode primary_node(&primary_region, true);
    primary_node.start_primary(12349);
    
    NetworkNode replica_node(&replica_region, false);
    replica_node.start_replica("127.0.0.1", 12349);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    char* replica_ptr = static_cast<char*>(replica_region.base_address());
    replica_node.request_page(replica_ptr);
    replica_ptr[0] = 111;
    
    // Primary stops. This should send SyncRequest, Replica should flush and send SyncResponse.
    // Primary stop() blocks until SyncResponse.
    primary_node.stop();
    
    char* primary_ptr = static_cast<char*>(primary_region.base_address());
    EXPECT_EQ(primary_ptr[0], 111);
    
    replica_node.stop();
}
