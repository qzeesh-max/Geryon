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
#include "geryon/recursive_spin_lock.hpp"
#include <thread>
#include <chrono>
#include <atomic>

using namespace geryon;

struct SharedState {
    recursive_spin_lock lock;
    int counter;
};

TEST(SynchronizationTest, DistributedSpinLock) {
    std::size_t size = MemoryRegion::system_page_size();
    
    MemoryRegion primary_region(size);
    primary_region.set_protection(primary_region.base_address(), size, PageProtection::ReadWrite);
    NetworkNode primary_node(&primary_region, true);
    
    MemoryRegion replica_region(size);
    replica_region.set_protection(replica_region.base_address(), size, PageProtection::None);
    NetworkNode replica_node(&replica_region, false);
    
    auto replica_fault = FaultHandler::create();
    replica_fault->register_region(replica_region.base_address(), size, [&](void* addr) -> bool {
        return replica_node.request_page(addr);
    });

    auto primary_fault = FaultHandler::create();
    primary_fault->register_region(primary_region.base_address(), size, [&](void* addr) -> bool {
        return primary_node.request_page(addr);
    });

    primary_node.start_primary(12353);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    replica_node.start_replica("127.0.0.1", 12353);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    SharedState* primary_state = new (primary_region.base_address()) SharedState();
    primary_state->counter = 0;

    SharedState* replica_state = static_cast<SharedState*>(replica_region.base_address());

    constexpr int INCREMENTS = 500;

    std::thread primary_thread([&]() {
        for (int i = 0; i < INCREMENTS; ++i) {
            primary_state->lock.lock();
            // Test recursion
            primary_state->lock.lock();
            primary_state->counter++;
            primary_state->lock.unlock();
            primary_state->lock.unlock();
            
            // Backoff so the other node can get ownership
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        std::cout << "Primary finished." << std::endl;
    });

    std::thread replica_thread([&]() {
        for (int i = 0; i < INCREMENTS; ++i) {
            replica_state->lock.lock();
            replica_state->lock.lock();
            replica_state->counter++;
            replica_state->lock.unlock();
            replica_state->lock.unlock();

            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        std::cout << "Replica finished." << std::endl;
    });

    primary_thread.join();
    replica_thread.join();

    EXPECT_EQ(primary_state->counter, INCREMENTS * 2);

    primary_fault->unregister_region(primary_region.base_address());
    replica_fault->unregister_region(replica_region.base_address());

    primary_node.stop();
    replica_node.stop();
}
