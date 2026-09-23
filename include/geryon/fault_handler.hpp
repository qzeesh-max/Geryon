#pragma once
#include <cstddef>
#include <functional>
#include <memory>

namespace geryon {

// A callback invoked when a page fault occurs in a registered memory region.
// It receives the faulting address.
// The callback must handle the fault (e.g. fetch data, update page protection to R/W)
// and return true to resume the faulting thread, or false to trigger a crash.
using PageFaultCallback = std::function<bool(void* fault_address)>;

class FaultHandler {
public:
    virtual ~FaultHandler() = default;

    virtual void register_region(void* base_address, std::size_t size, PageFaultCallback callback) = 0;
    virtual void unregister_region(void* base_address) = 0;

    // Factory method to get the platform-specific fault handler instance
    static std::unique_ptr<FaultHandler> create();
};

} // namespace geryon
