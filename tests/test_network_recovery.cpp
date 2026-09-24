#include <gtest/gtest.h>
#include "geryon/memory_region.hpp"
#include "geryon/network_node.hpp"
#include "geryon/fault_handler.hpp"
#include <thread>
#include <chrono>
#include <atomic>

using namespace geryon;

class NetworkRecoveryTest : public ::testing::Test {
};

TEST_F(NetworkRecoveryTest, PrimaryRecoversWhenReplicaDisconnects) {
    std::size_t region_size = 4 * MemoryRegion::system_page_size();
    uint16_t port = 19056;

    MemoryRegion primary_region(region_size);
    primary_region.set_protection(primary_region.base_address(), region_size, PageProtection::ReadWrite);
    NetworkNode primary_node(&primary_region, true);
    primary_node.start_primary(port);
    
    auto primary_fault = FaultHandler::create();
    primary_fault->register_region_with_io(primary_region.base_address(), primary_region.io_address(), primary_region.size(), [&](void* addr) -> bool {
        return primary_node.request_page(addr);
    });

    int* primary_data = static_cast<int*>(primary_region.base_address());
    primary_data[0] = 42; // Primary has page 0

    std::atomic<bool> replica_read_succeeded{false};

    // We use a pointer to easily destroy the replica and force a disconnect
    auto replica_thread_func = [&]() {
        MemoryRegion replica_region(region_size);
        replica_region.set_protection(replica_region.base_address(), region_size, PageProtection::None);
        NetworkNode replica_node(&replica_region, false);
        replica_node.start_replica("127.0.0.1", port);

        auto replica_fault = FaultHandler::create();
        replica_fault->register_region_with_io(replica_region.base_address(), replica_region.io_address(), replica_region.size(), [&](void* addr) -> bool {
            return replica_node.request_page(addr);
        });

        int* replica_data = static_cast<int*>(replica_region.base_address());

        // Replica steals page 0
        int val = replica_data[0];
        if (val == 42) {
            replica_read_succeeded = true;
        }

        // Wait to be destroyed
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        replica_fault->unregister_region(replica_region.base_address());
        replica_node.stop(); // Graceful shutdown
    };

    std::thread replica_thread(replica_thread_func);

    // Wait until replica has stolen the page
    while (!replica_read_succeeded) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Now the Primary DOES NOT HAVE page 0.
    // The replica thread will exit and close its connection.
    replica_thread.join();

    // Give the primary a moment to process the disconnect
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // The Primary should have recovered page 0 locally since the replica disconnected!
    // Try to read/write it. This should NOT block indefinitely!
    primary_data[0] = 100;
    
    EXPECT_EQ(primary_data[0], 100);

    primary_fault->unregister_region(primary_region.base_address());
}
