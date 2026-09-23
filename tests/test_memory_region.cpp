#include <gtest/gtest.h>
#include "geryon/memory_region.hpp"

using namespace geryon;

TEST(MemoryRegionTest, AllocationAndPageSize) {
    std::size_t page_size = MemoryRegion::system_page_size();
    EXPECT_GT(page_size, 0);

    // Requesting 1 byte should allocate at least 1 page
    MemoryRegion region(1);
    EXPECT_EQ(region.size(), page_size);
    EXPECT_NE(region.base_address(), nullptr);
}

TEST(MemoryRegionTest, ProtectionChanges) {
    std::size_t page_size = MemoryRegion::system_page_size();
    MemoryRegion region(page_size * 2);

    // It should start with PageProtection::None, so writing to it should segfault.
    // We cannot easily test segfaults in a normal gtest without EXPECT_DEATH,
    // which forks the process. 
    EXPECT_DEATH({
        char* ptr = static_cast<char*>(region.base_address());
        ptr[0] = 'A'; // Should crash
    }, "");

    // Change protection to ReadWrite
    region.set_protection(region.base_address(), page_size, PageProtection::ReadWrite);

    // Now it should be writable
    char* ptr = static_cast<char*>(region.base_address());
    ptr[0] = 'A';
    EXPECT_EQ(ptr[0], 'A');

    // Second page should still be inaccessible
    EXPECT_DEATH({
        ptr[page_size] = 'B'; // Should crash
    }, "");
}
