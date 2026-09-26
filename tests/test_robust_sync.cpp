/*
 * Geryon - A Distributed Shared Memory Framework
 * Copyright (C) 2026 Zeeshan Qazi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */
#include <gtest/gtest.h>
#include "geryon/memory_region.hpp"
#include "geryon/network_node.hpp"
#include "geryon/fault_handler.hpp"
#include "geryon/synchronization.hpp"
#include <thread>
#include <chrono>
#include <iostream>
#include <atomic>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace geryon;

struct SharedState {
    sync::ticket_lock t_lock;
    sync::epoch_lock e_lock;
    sync::mcs_lock m_lock;
    std::atomic<int> data;
};

int run_robust_sync_replica(uint16_t port, int test_case) {
    auto r = std::make_unique<MemoryRegion>(MemoryRegion::system_page_size() * 10);
    r->set_protection(r->base_address(), r->size(), PageProtection::None);

    auto n = std::make_unique<NetworkNode>(r.get(), false);
    auto fh = FaultHandler::create();
    fh->register_region_with_io(r->base_address(), r->io_address(), r->size(), [&n](void* addr) -> bool {
        return n->request_page(addr);
    });

    n->start_replica("127.0.0.1", port, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    SharedState* state = reinterpret_cast<SharedState*>(r->base_address());

    if (test_case == 1) { // Ticket Lock
        state->t_lock.lock();
        state->data = 42;
        while(true) std::this_thread::sleep_for(std::chrono::seconds(1));
    } else if (test_case == 2) { // Epoch Lock
        state->e_lock.lock();
        state->data = 42;
        while(true) std::this_thread::sleep_for(std::chrono::seconds(1));
    } else if (test_case == 3) { // Epoch Lock Timeout
        state->e_lock.lock();
        state->data = 42;
        while(true) std::this_thread::sleep_for(std::chrono::seconds(1)); // Alive but no heartbeat
    } else if (test_case == 4) { // MCS Lock
        state->m_lock.lock();
        state->data = 42;
        while(true) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    return 0;
}

class RobustSyncTest : public ::testing::TestWithParam<int> {
protected:
    std::unique_ptr<MemoryRegion> region;
    std::unique_ptr<NetworkNode> network_node;
    std::unique_ptr<FaultHandler> fault_handler;
    uint16_t port = 16000;

    void SetUp() override {
        port += (std::chrono::system_clock::now().time_since_epoch().count() % 1000);
        
        region = std::make_unique<MemoryRegion>(MemoryRegion::system_page_size() * 10);
        region->set_protection(region->base_address(), region->size(), PageProtection::ReadWrite);

        network_node = std::make_unique<NetworkNode>(region.get(), true);
        fault_handler = FaultHandler::create();

        fault_handler->register_region_with_io(
            region->base_address(), region->io_address(), region->size(),
            [this](void* addr) -> bool { return network_node->request_page(addr); });
    }

    void TearDown() override {
        if (fault_handler && region) {
            fault_handler->unregister_region(region->base_address());
        }
        network_node->stop();
        network_node.reset();
        fault_handler.reset();
        region.reset();
    }
};

TEST_P(RobustSyncTest, RecoveryAfterCrashOrTimeout) {
    int test_case = GetParam();
    
    const auto& args = testing::internal::GetArgvs();
    bool is_replica = false;
    uint16_t replica_port = 0;
    int replica_case = 0;
    for (const auto& arg : args) {
        if (arg == "--run_as_robust_sync_replica") {
            is_replica = true;
        } else if (arg.find("--replica_port=") == 0) {
            replica_port = std::stoi(arg.substr(15));
        } else if (arg.find("--replica_case=") == 0) {
            replica_case = std::stoi(arg.substr(15));
        }
    }

    if (is_replica && replica_case == test_case) {
        std::exit(run_robust_sync_replica(replica_port, replica_case));
    } else if (is_replica) {
        return; // Ignore
    }

    std::string exec_path = args[0];

    network_node->start_primary(port);

    SharedState* state = reinterpret_cast<SharedState*>(region->base_address());
    new (state) SharedState();
    state->data = 0;

    std::string cmd = "\"" + exec_path + "\" --gtest_filter=*RobustSyncTest.RecoveryAfterCrashOrTimeout* --run_as_robust_sync_replica --replica_port=" + std::to_string(port) + " --replica_case=" + std::to_string(test_case);

#ifdef _WIN32
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    std::string args_cmd = cmd;
    if (!CreateProcessA(NULL, &args_cmd[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        FAIL() << "CreateProcessA failed";
    }
#else
    pid_t pid = fork();
    if (pid == 0) {
        std::exit(run_robust_sync_replica(port, test_case));
    }
#endif

    // Wait for the child to grab the lock and write 42
    auto start_wait_replica = std::chrono::steady_clock::now();
    while (state->data != 42) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start_wait_replica > std::chrono::seconds(5)) {
            break; 
        }
    }
    EXPECT_EQ(state->data, 42);

    std::atomic<bool> lock_acquired{false};
    std::atomic<bool> owner_died_flag{false};
    
    std::thread acquirer([&]() {
        robust_lock_status status;
        if (test_case == 1) status = state->t_lock.lock();
        else if (test_case == 2 || test_case == 3) status = state->e_lock.lock(1000); 
        else if (test_case == 4) status = state->m_lock.lock();

        if (status == robust_lock_status::owner_died) {
            owner_died_flag = true;
        }
        lock_acquired = true;
        
        if (test_case == 1) state->t_lock.unlock();
        else if (test_case == 2 || test_case == 3) state->e_lock.unlock();
        else if (test_case == 4) state->m_lock.unlock();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_FALSE(lock_acquired.load());

    if (test_case == 3) {
        std::cout << "Waiting for timeout stealing..." << std::endl;
        
        auto start_wait = std::chrono::steady_clock::now();
        bool recovered = false;
        while (std::chrono::steady_clock::now() - start_wait < std::chrono::seconds(3)) {
            if (lock_acquired.load()) {
                recovered = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        EXPECT_TRUE(recovered) << "Failed to steal lock after timeout";
        EXPECT_TRUE(owner_died_flag.load()) << "Expected lock acquisition to report owner_died";
        
#ifdef _WIN32
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
#else
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
#endif

    } else {
        std::cout << "Primary killing replica process..." << std::endl;
#ifdef _WIN32
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
#else
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
#endif

        auto start_wait = std::chrono::steady_clock::now();
        bool recovered = false;
        while (std::chrono::steady_clock::now() - start_wait < std::chrono::seconds(5)) {
            if (lock_acquired.load()) {
                recovered = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        EXPECT_TRUE(recovered) << "Failed to recover the lock after replica crash";
        if (test_case != 1) {
            EXPECT_TRUE(owner_died_flag.load()) << "Expected lock acquisition to report owner_died";
        } else {
            EXPECT_FALSE(owner_died_flag.load());
        }
    }

    acquirer.join();
}

INSTANTIATE_TEST_SUITE_P(
    AllLocks,
    RobustSyncTest,
    ::testing::Values(1, 2, 3, 4)
);
