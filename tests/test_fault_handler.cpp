#include <gtest/gtest.h>
#include "geryon/memory_region.hpp"
#include "geryon/fault_handler.hpp"

using namespace geryon;

TEST(FaultHandlerTest, SoftFaultHandling) {
    std::size_t page_size = MemoryRegion::system_page_size();
    MemoryRegion region(page_size * 2);
    
    auto fault_handler = FaultHandler::create();
    
    bool fault_handled = false;
    
    // Register a callback
    fault_handler->register_region(region.base_address(), region.size(), [&](void* addr) -> bool {
        fault_handled = true;
        
        // Find which page faulted
        char* base = static_cast<char*>(region.base_address());
        char* ptr = static_cast<char*>(addr);
        std::size_t offset = ptr - base;
        std::size_t page_index = offset / page_size;
        
        void* page_base = base + (page_index * page_size);
        
        // Change protection to ReadWrite
        region.set_protection(page_base, page_size, PageProtection::ReadWrite);
        
        // Write some data to prove we intercepted it
        char* p = static_cast<char*>(page_base);
        p[0] = 'F';
        
        return true; // Successfully handled
    });
    
    // Access the memory. This should trigger a soft fault.
    char* ptr = static_cast<char*>(region.base_address());
    
    // It should initially be PROT_NONE, so writing triggers fault handler.
    // Fault handler sets it to R/W and sets ptr[0] = 'F'.
    // Then the instruction is retried.
    
    ptr[0] = 'Z'; 
    
    EXPECT_TRUE(fault_handled);
    EXPECT_EQ(ptr[0], 'Z'); // The retry overwrites 'F' with 'Z'
    
    fault_handler->unregister_region(region.base_address());
}
