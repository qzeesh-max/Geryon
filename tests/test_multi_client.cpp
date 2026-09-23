#include <gtest/gtest.h>
#include "geryon/memory_region.hpp"
#include "geryon/fault_handler.hpp"
#include "geryon/network_node.hpp"
#include "geryon/recursive_spin_lock.hpp"
#include <thread>
#include <vector>
#include <chrono>

using namespace geryon;

struct SharedCounter {
    recursive_spin_lock lock;
    int count;
};

TEST(MultiClientTest, StarTopologyBouncing) {
    std::size_t region_size = MemoryRegion::system_page_size(); // Need at least one page
    uint16_t port = 13579;
    const int NUM_REPLICAS = 3;
    const int INCREMENTS_PER_NODE = 200;

    // 1. Setup Primary
    MemoryRegion primary_region(region_size);
    primary_region.set_protection(primary_region.base_address(), region_size, PageProtection::ReadWrite);
    NetworkNode primary_node(&primary_region, true);
    
    auto primary_fault = FaultHandler::create();
    primary_fault->register_region(primary_region.base_address(), primary_region.size(), [&](void* addr) -> bool {
        return primary_node.request_page(addr);
    });
    primary_node.start_primary(port);

    SharedCounter* primary_counter = new (primary_region.base_address()) SharedCounter();
    primary_counter->count = 0;

    // 2. Setup Replicas
    std::vector<std::unique_ptr<MemoryRegion>> replica_regions;
    std::vector<std::unique_ptr<NetworkNode>> replica_nodes;
    std::vector<std::unique_ptr<FaultHandler>> replica_faults;

    for (int i = 0; i < NUM_REPLICAS; ++i) {
        replica_regions.push_back(std::make_unique<MemoryRegion>(region_size));
        replica_regions.back()->set_protection(replica_regions.back()->base_address(), region_size, PageProtection::None);
        
        replica_nodes.push_back(std::make_unique<NetworkNode>(replica_regions.back().get(), false));
        
        auto replica_fault = FaultHandler::create();
        replica_fault->register_region(replica_regions.back()->base_address(), replica_regions.back()->size(), [node = replica_nodes.back().get()](void* addr) -> bool {
            return node->request_page(addr);
        });
        replica_faults.push_back(std::move(replica_fault));
        
        replica_nodes.back()->start_replica("127.0.0.1", port);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // wait for connections

    // 3. Start incrementing threads on all nodes
    std::vector<std::thread> threads;

    auto increment_task = [INCREMENTS_PER_NODE](void* base_addr) {
        SharedCounter* c = reinterpret_cast<SharedCounter*>(base_addr);
        for (int i = 0; i < INCREMENTS_PER_NODE; ++i) {
            c->lock.lock();
            c->count++;
            c->lock.unlock();
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    };

    // Primary thread
    threads.emplace_back(increment_task, primary_region.base_address());

    // Replica threads
    for (int i = 0; i < NUM_REPLICAS; ++i) {
        threads.emplace_back(increment_task, replica_regions[i]->base_address());
    }

    // 4. Wait for all
    for (auto& t : threads) {
        t.join();
    }

    // 5. Verify the counter
    // The primary should have the page locally at the end if it was the last to run,
    // but we can just ask the primary to acquire the lock to fetch the page and check.
    primary_counter->lock.lock();
    EXPECT_EQ(primary_counter->count, (NUM_REPLICAS + 1) * INCREMENTS_PER_NODE);
    primary_counter->lock.unlock();

    // 6. Cleanup
    primary_node.stop();
    for (auto& n : replica_nodes) n->stop();
    
    primary_fault->unregister_region(primary_region.base_address());
    for (size_t i = 0; i < replica_regions.size(); ++i) {
        replica_faults[i]->unregister_region(replica_regions[i]->base_address());
    }
}
