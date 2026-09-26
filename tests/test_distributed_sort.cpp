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
#include <thread>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <random>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace geryon;

const int NUM_ELEMENTS = 20000000;
const int NUM_REPLICAS = 3;
const int TOTAL_NODES = NUM_REPLICAS + 1;

// Align chunks to 16384 bytes (4096 elements). 4997120 elements = 1220 pages of 16KB.
const int ELEMENTS_PER_NODE = 4997120;

struct NodeFlags {
    std::atomic<int> ready{0};
    std::atomic<int> done{0};
    char padding[16384 - 2 * sizeof(std::atomic<int>)];
};

struct SortState {
    NodeFlags flags[TOTAL_NODES];
    uint32_t data[NUM_ELEMENTS];
};

void run_replica(uint16_t port, int node_index) {
    std::size_t region_size = ((sizeof(SortState) + MemoryRegion::system_page_size() - 1) / MemoryRegion::system_page_size()) * MemoryRegion::system_page_size();
    MemoryRegion replica_region(region_size);
    replica_region.set_protection(replica_region.base_address(), region_size, PageProtection::None);
    NetworkNode replica_node(&replica_region, false);
    
    auto replica_fault = FaultHandler::create();
    replica_fault->register_region_with_io(replica_region.base_address(), replica_region.io_address(), region_size, [&](void* addr) -> bool {
        return replica_node.request_page(addr);
    });

    replica_node.start_replica("127.0.0.1", port);

    SortState* state = reinterpret_cast<SortState*>(replica_region.base_address());
    
    // Announce ready
    state->flags[node_index].ready = 1;

    // Sort our chunk
    int start_idx = node_index * ELEMENTS_PER_NODE;
    int end_idx = (node_index == TOTAL_NODES - 1) ? NUM_ELEMENTS : start_idx + ELEMENTS_PER_NODE;
    
    std::sort(state->data + start_idx, state->data + end_idx);
    
    // Announce done
    state->flags[node_index].done = 1;

    // Wait for Primary to finish verification
    while (state->flags[0].done != 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto stats = replica_node.get_statistics();
    double avg_transfer = stats.pages_sent > 0 ? static_cast<double>(stats.total_transfer_time_us) / stats.pages_sent : 0.0;
    double avg_between = stats.pages_sent > 1 ? static_cast<double>(stats.total_time_between_transfers_us) / (stats.pages_sent - 1) : 0.0;
    std::cout << "[Replica " << node_index << "] Pages Sent: " << stats.pages_sent 
              << ", Pages Received: " << stats.pages_received 
              << " | Avg Transfer Time: " << avg_transfer << " us"
              << " | Avg Time Between Transfers: " << avg_between << " us" << std::endl;

    replica_node.stop();
    replica_fault->unregister_region(replica_region.base_address());
}

TEST(DistributedSortTest, MultiprocessPiecewiseSort) {
    const auto& args = testing::internal::GetArgvs();
    bool is_replica = false;
    uint16_t replica_port = 0;
    int replica_index = 0;
    
    for (const auto& arg : args) {
        if (arg == "--run_as_replica") {
            is_replica = true;
        } else if (arg.find("--replica_port=") == 0) {
            replica_port = static_cast<uint16_t>(std::stoi(arg.substr(13)));
        } else if (arg.find("--replica_index=") == 0) {
            replica_index = std::stoi(arg.substr(14));
        }
    }

    if (is_replica) {
        run_replica(replica_port, replica_index);
        std::exit(0);
    }

    std::size_t region_size = ((sizeof(SortState) + MemoryRegion::system_page_size() - 1) / MemoryRegion::system_page_size()) * MemoryRegion::system_page_size();
    uint16_t port = 15685;

    MemoryRegion primary_region(region_size);
    primary_region.set_protection(primary_region.base_address(), region_size, PageProtection::ReadWrite);
    
    SortState* state = new (primary_region.base_address()) SortState();
    
    // Generate data
    std::mt19937 gen(42);
    for (int i = 0; i < NUM_ELEMENTS; ++i) {
        state->data[i] = gen();
    }

    NetworkNode primary_node(&primary_region, true);
    auto primary_fault = FaultHandler::create();
    primary_fault->register_region_with_io(primary_region.base_address(), primary_region.io_address(), region_size, [&](void* addr) -> bool {
        return primary_node.request_page(addr);
    });
    
    primary_node.start_primary(port);

    std::string exec_path = args[0];
    std::vector<std::thread> process_waiters;
    for (int i = 1; i <= NUM_REPLICAS; ++i) {
        process_waiters.emplace_back([=]() {
#ifdef _WIN32
            std::string cmd = "\"" + exec_path + "\" --gtest_filter=DistributedSortTest.MultiprocessPiecewiseSort --run_as_replica"
                            + " --replica_port=" + std::to_string(port)
                            + " --replica_index=" + std::to_string(i);
            STARTUPINFOA si;
            PROCESS_INFORMATION pi;
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            ZeroMemory(&pi, sizeof(pi));
            std::string args_cmd = cmd;
            if (!CreateProcessA(NULL, &args_cmd[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                throw std::runtime_error("CreateProcessA failed");
            }
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
#else
            std::string cmd = exec_path + " --gtest_filter=DistributedSortTest.MultiprocessPiecewiseSort --run_as_replica"
                            + " --replica_port=" + std::to_string(port)
                            + " --replica_index=" + std::to_string(i) + " &";
            std::system(cmd.c_str());
#endif
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Wait for replicas to be ready
    for (int i = 1; i <= NUM_REPLICAS; ++i) {
        while (state->flags[i].ready == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // Primary sorts its own chunk
    int start_idx = 0;
    int end_idx = ELEMENTS_PER_NODE;
    std::sort(state->data + start_idx, state->data + end_idx);
    state->flags[0].done = 1;

    // Wait for replicas to finish sorting
    for (int i = 1; i <= NUM_REPLICAS; ++i) {
        while (state->flags[i].done == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // Sync all pages back to primary to ensure consistent view before final validation
    primary_node.trigger_synchronization();

    // Final merge / validation
    // First, verify that each chunk is sorted locally
    for (int i = 0; i < TOTAL_NODES; ++i) {
        int chunk_start = i * ELEMENTS_PER_NODE;
        int chunk_end = (i == TOTAL_NODES - 1) ? NUM_ELEMENTS : chunk_start + ELEMENTS_PER_NODE;
        EXPECT_TRUE(std::is_sorted(state->data + chunk_start, state->data + chunk_end));
    }

    // Full sort (merge the 4 pieces)
    std::sort(state->data, state->data + NUM_ELEMENTS);
    
    EXPECT_TRUE(std::is_sorted(state->data, state->data + NUM_ELEMENTS));

    auto stats = primary_node.get_statistics();
    double avg_transfer = stats.pages_sent > 0 ? static_cast<double>(stats.total_transfer_time_us) / stats.pages_sent : 0.0;
    double avg_between = stats.pages_sent > 1 ? static_cast<double>(stats.total_time_between_transfers_us) / (stats.pages_sent - 1) : 0.0;
    std::cout << "[Primary] Pages Sent: " << stats.pages_sent 
              << ", Pages Received: " << stats.pages_received 
              << " | Avg Transfer Time: " << avg_transfer << " us"
              << " | Avg Time Between Transfers: " << avg_between << " us" << std::endl;

    // Release replicas
    state->flags[0].done = 2;

    for (auto& t : process_waiters) t.join();
    
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    primary_node.stop();
    primary_fault->unregister_region(primary_region.base_address());
}
