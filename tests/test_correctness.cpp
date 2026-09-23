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
#include "geryon/fault_handler.hpp"
#include "geryon/network_node.hpp"
#include "geryon/recursive_spin_lock.hpp"
#include <thread>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <string>

using namespace geryon;

struct SharedCounter {
    recursive_spin_lock lock;
    int count;
};

// ---------------------------------------------------------
// Multithread Multipage Correctness (Single Process)
// ---------------------------------------------------------
TEST(CorrectnessTest, MultithreadMultipageCorrectness) {
    const std::size_t num_pages = 4;
    std::size_t region_size = MemoryRegion::system_page_size() * num_pages; 
    uint16_t port = 15682;
    const int NUM_REPLICAS = 3;
    const int THREADS_PER_NODE = 4;
    const int INCREMENTS_PER_THREAD = 200;

    // 1. Setup Primary
    MemoryRegion primary_region(region_size);
    primary_region.set_protection(primary_region.base_address(), region_size, PageProtection::ReadWrite);
    NetworkNode primary_node(&primary_region, true);
    auto primary_fault = FaultHandler::create();
    primary_fault->register_region(primary_region.base_address(), region_size, [&](void* addr) -> bool {
        return primary_node.request_page(addr);
    });
    primary_node.start_primary(port);

    // Initialize counters across pages
    std::vector<SharedCounter*> counters(num_pages);
    for (size_t i = 0; i < num_pages; ++i) {
        void* page_addr = static_cast<char*>(primary_region.base_address()) + (i * MemoryRegion::system_page_size());
        counters[i] = new (page_addr) SharedCounter();
        counters[i]->count = 0;
    }

    // 2. Setup Replicas
    std::vector<std::unique_ptr<MemoryRegion>> replica_regions;
    std::vector<std::unique_ptr<NetworkNode>> replica_nodes;
    std::vector<std::unique_ptr<FaultHandler>> replica_faults;

    for (int i = 0; i < NUM_REPLICAS; ++i) {
        replica_regions.push_back(std::make_unique<MemoryRegion>(region_size));
        replica_regions.back()->set_protection(replica_regions.back()->base_address(), region_size, PageProtection::None);
        replica_nodes.push_back(std::make_unique<NetworkNode>(replica_regions.back().get(), false));
        
        auto replica_fault = FaultHandler::create();
        replica_fault->register_region(replica_regions.back()->base_address(), region_size, [node = replica_nodes.back().get()](void* addr) -> bool {
            return node->request_page(addr);
        });
        replica_faults.push_back(std::move(replica_fault));
        replica_nodes.back()->start_replica("127.0.0.1", port);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 3. Multithreaded execution
    auto increment_task = [&](void* base_addr) {
        for (int i = 0; i < INCREMENTS_PER_THREAD; ++i) {
            int target_page = (i + std::hash<std::thread::id>{}(std::this_thread::get_id())) % num_pages;
            SharedCounter* c = reinterpret_cast<SharedCounter*>(static_cast<char*>(base_addr) + target_page * MemoryRegion::system_page_size());
            c->lock.lock();
            c->count++;
            c->lock.unlock();
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS_PER_NODE; ++t) {
        threads.emplace_back(increment_task, primary_region.base_address());
    }
    for (int i = 0; i < NUM_REPLICAS; ++i) {
        for (int t = 0; t < THREADS_PER_NODE; ++t) {
            threads.emplace_back(increment_task, replica_regions[i]->base_address());
        }
    }

    for (auto& t : threads) t.join();

    // 4. Verify Correctness
    primary_node.trigger_synchronization();

    int total_increments = 0;
    for (size_t i = 0; i < num_pages; ++i) {
        void* page_addr = static_cast<char*>(primary_region.base_address()) + (i * MemoryRegion::system_page_size());
        SharedCounter* c = reinterpret_cast<SharedCounter*>(page_addr);
        total_increments += c->count;
    }

    int expected = (1 + NUM_REPLICAS) * THREADS_PER_NODE * INCREMENTS_PER_THREAD;
    EXPECT_EQ(total_increments, expected);

    // 5. Cleanup
    for (auto& node : replica_nodes) node->stop();
    primary_node.stop();

    primary_fault->unregister_region(primary_region.base_address());
    for (size_t i = 0; i < replica_regions.size(); ++i) {
        replica_faults[i]->unregister_region(replica_regions[i]->base_address());
    }
}

// ---------------------------------------------------------
// Multiprocess Multipage Correctness (Via Subprocesses)
// ---------------------------------------------------------

void run_multiprocess_replica(uint16_t port) {
    const std::size_t num_pages = 4;
    std::size_t region_size = MemoryRegion::system_page_size() * num_pages; 
    const int THREADS_PER_NODE = 4;
    const int INCREMENTS_PER_THREAD = 200;

    MemoryRegion replica_region(region_size);
    replica_region.set_protection(replica_region.base_address(), region_size, PageProtection::None);
    NetworkNode replica_node(&replica_region, false);
    
    auto replica_fault = FaultHandler::create();
    replica_fault->register_region(replica_region.base_address(), region_size, [&](void* addr) -> bool {
        return replica_node.request_page(addr);
    });

    replica_node.start_replica("127.0.0.1", port);

    auto increment_task = [&](void* base_addr) {
        for (int i = 0; i < INCREMENTS_PER_THREAD; ++i) {
            int target_page = (i + std::hash<std::thread::id>{}(std::this_thread::get_id())) % num_pages;
            SharedCounter* c = reinterpret_cast<SharedCounter*>(static_cast<char*>(base_addr) + target_page * MemoryRegion::system_page_size());
            c->lock.lock();
            c->count++;
            c->lock.unlock();
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS_PER_NODE; ++t) {
        threads.emplace_back(increment_task, replica_region.base_address());
    }

    for (auto& t : threads) t.join();

    replica_node.stop();
    replica_fault->unregister_region(replica_region.base_address());
}

TEST(CorrectnessTest, MultiprocessMultipageCorrectness) {
    const auto& args = testing::internal::GetArgvs();
    bool is_replica = false;
    uint16_t replica_port = 0;
    for (const auto& arg : args) {
        if (arg == "--run_as_replica") {
            is_replica = true;
        } else if (arg.find("--replica_port=") == 0) {
            replica_port = static_cast<uint16_t>(std::stoi(arg.substr(13)));
        }
    }

    if (is_replica) {
        run_multiprocess_replica(replica_port);
        std::exit(0);
    }

    const std::size_t num_pages = 4;
    std::size_t region_size = MemoryRegion::system_page_size() * num_pages; 
    uint16_t port = 15683;
    const int NUM_REPLICAS = 3;
    const int THREADS_PER_NODE = 4;
    const int INCREMENTS_PER_THREAD = 200;

    MemoryRegion primary_region(region_size);
    primary_region.set_protection(primary_region.base_address(), region_size, PageProtection::ReadWrite);
    NetworkNode primary_node(&primary_region, true);
    auto primary_fault = FaultHandler::create();
    primary_fault->register_region(primary_region.base_address(), region_size, [&](void* addr) -> bool {
        return primary_node.request_page(addr);
    });
    primary_node.start_primary(port);

    std::vector<SharedCounter*> counters(num_pages);
    for (size_t i = 0; i < num_pages; ++i) {
        void* page_addr = static_cast<char*>(primary_region.base_address()) + (i * MemoryRegion::system_page_size());
        counters[i] = new (page_addr) SharedCounter();
        counters[i]->count = 0;
    }

    std::string exec_path = args[0];
    std::vector<std::thread> process_waiters;
    for (int i = 0; i < NUM_REPLICAS; ++i) {
        process_waiters.emplace_back([=]() {
            std::string cmd = exec_path + " --gtest_filter=CorrectnessTest.MultiprocessMultipageCorrectness --run_as_replica --replica_port=" + std::to_string(port);
            int ret = std::system(cmd.c_str());
            EXPECT_EQ(ret, 0);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    auto increment_task = [&](void* base_addr) {
        for (int i = 0; i < INCREMENTS_PER_THREAD; ++i) {
            int target_page = (i + std::hash<std::thread::id>{}(std::this_thread::get_id())) % num_pages;
            SharedCounter* c = reinterpret_cast<SharedCounter*>(static_cast<char*>(base_addr) + target_page * MemoryRegion::system_page_size());
            c->lock.lock();
            c->count++;
            c->lock.unlock();
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    };

    std::vector<std::thread> primary_threads;
    for (int t = 0; t < THREADS_PER_NODE; ++t) {
        primary_threads.emplace_back(increment_task, primary_region.base_address());
    }

    for (auto& t : primary_threads) t.join();
    for (auto& t : process_waiters) t.join();

    primary_node.trigger_synchronization();

    int total_increments = 0;
    for (size_t i = 0; i < num_pages; ++i) {
        void* page_addr = static_cast<char*>(primary_region.base_address()) + (i * MemoryRegion::system_page_size());
        SharedCounter* c = reinterpret_cast<SharedCounter*>(page_addr);
        total_increments += c->count;
    }

    int expected = (1 + NUM_REPLICAS) * THREADS_PER_NODE * INCREMENTS_PER_THREAD;
    EXPECT_EQ(total_increments, expected);

    primary_node.stop();
    primary_fault->unregister_region(primary_region.base_address());
}
