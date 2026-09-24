#include <gtest/gtest.h>
#include "geryon/memory_region.hpp"
#include "geryon/network_node.hpp"
#include "geryon/fault_handler.hpp"
#include <thread>
#include <chrono>
#include <atomic>
#include <cstdlib>

using namespace geryon;

class ReadOnlyReplicaTest : public ::testing::Test {
};

TEST_F(ReadOnlyReplicaTest, ReplicaReadsOnlyUntilPrimaryDies) {
    std::size_t region_size = 4 * MemoryRegion::system_page_size();
    uint16_t port = 19055;

    MemoryRegion primary_region(region_size);
    primary_region.set_protection(primary_region.base_address(), region_size, PageProtection::ReadWrite);
    NetworkNode primary_node(&primary_region, true);
    primary_node.start_primary(port);
    
    auto primary_fault = FaultHandler::create();
    primary_fault->register_region_with_io(primary_region.base_address(), primary_region.io_address(), primary_region.size(), [&](void* addr) -> bool {
        return primary_node.request_page(addr);
    });

    // Primary writes some data
    int* primary_data = static_cast<int*>(primary_region.base_address());
    primary_data[0] = 42;
    primary_data[1024] = 84;

    std::atomic<bool> replica_write_succeeded{false};
    std::atomic<bool> replica_read_succeeded{false};
    std::atomic<int> replica_read_value{0};

    std::thread replica_thread([&]() {
        MemoryRegion replica_region(region_size);
        replica_region.set_protection(replica_region.base_address(), region_size, PageProtection::None);
        NetworkNode replica_node(&replica_region, false);
        replica_node.start_replica("127.0.0.1", port, true); // read_only = true

        auto replica_fault = FaultHandler::create();
        replica_fault->register_region_with_io(replica_region.base_address(), replica_region.io_address(), replica_region.size(), [&](void* addr) -> bool {
            return replica_node.request_page(addr);
        });

        int* replica_data = static_cast<int*>(replica_region.base_address());

        // Should be able to read just fine
        replica_read_value = replica_data[0];
        replica_read_succeeded = true;

        // Try to write. This should BLOCK because it's a readonly replica,
        // until the primary dies.
        replica_data[1024] = 999;
        replica_write_succeeded = true;

        replica_fault->unregister_region(replica_region.base_address());
    });

    // Wait until replica has successfully read
    while (!replica_read_succeeded) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(replica_read_value.load(), 42);

    // Give replica some time to hit the write fault
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // The replica write should NOT have succeeded yet!
    EXPECT_FALSE(replica_write_succeeded.load());

    // Now, kill the primary
    primary_node.stop();

    // After primary dies, replica should promote to R/W and complete the write
    replica_thread.join();

    EXPECT_TRUE(replica_write_succeeded.load());
    primary_fault->unregister_region(primary_region.base_address());
}
