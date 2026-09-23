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

#include <benchmark/benchmark.h>
#include "geryon/memory_region.hpp"
#include "geryon/network_node.hpp"
#include "geryon/fault_handler.hpp"
#include <thread>
#include <chrono>

using namespace geryon;

static void BM_PageTransfer(benchmark::State& state) {
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

    primary_node.start_primary(12347);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    replica_node.start_replica("127.0.0.1", 12347);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    volatile int* primary_ptr = static_cast<volatile int*>(primary_region.base_address());
    volatile int* replica_ptr = static_cast<volatile int*>(replica_region.base_address());

    *primary_ptr = 1;

    for (auto _ : state) {
        // Replica reads (causes page to transfer to replica)
        int val1 = *replica_ptr;
        benchmark::DoNotOptimize(val1);

        // Primary reads (causes page to transfer back to primary)
        int val2 = *primary_ptr;
        benchmark::DoNotOptimize(val2);
    }

    primary_fault->unregister_region(primary_region.base_address());
    replica_fault->unregister_region(replica_region.base_address());

    primary_node.stop();
    replica_node.stop();
}

BENCHMARK(BM_PageTransfer)->Unit(benchmark::kMicrosecond);
BENCHMARK_MAIN();
