#include <gtest/gtest.h>
#include "geryon/memory_region.hpp"
#include "geryon/fault_handler.hpp"
#include "geryon/network_node.hpp"
#include "geryon/robust_spin_lock.hpp"
#include <thread>
#include <chrono>
#include <iostream>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#endif

using namespace geryon;

struct SharedState {
    robust_spin_lock lock;
    int data;
};

class RobustLockTest : public ::testing::Test {
protected:
    void SetUp() override {
        port = 9010 + (std::rand() % 100);
        
        region = std::make_unique<MemoryRegion>(MemoryRegion::system_page_size() * 10);
        region->set_protection(region->base_address(), region->size(), PageProtection::ReadWrite);
        
        network_node = std::make_unique<NetworkNode>(region.get(), true);
        
        fault_handler = FaultHandler::create();
        fault_handler->register_region_with_io(region->base_address(), region->io_address(), region->size(), [this](void* addr) -> bool {
            return network_node->request_page(addr);
        });
    }

    void TearDown() override {
        network_node->stop();
        network_node.reset();
        fault_handler.reset();
        region.reset();
    }

    uint16_t port;
    std::unique_ptr<MemoryRegion> region;
    std::unique_ptr<FaultHandler> fault_handler;
    std::unique_ptr<NetworkNode> network_node;
};

// -------------------------------------------------------------------------------------------------
// Subprocess worker that grabs the lock and loops infinitely
// -------------------------------------------------------------------------------------------------
int run_replica_that_grabs_lock(uint16_t port) {
    auto r = std::make_unique<MemoryRegion>(MemoryRegion::system_page_size() * 10);
    r->set_protection(r->base_address(), r->size(), PageProtection::None);

    auto n = std::make_unique<NetworkNode>(r.get(), false);

    auto fh = FaultHandler::create();
    fh->register_region_with_io(r->base_address(), r->io_address(), r->size(), [&n](void* addr) -> bool {
        return n->request_page(addr);
    });

    n->start_replica("127.0.0.1", port, false);
    
    // Wait for cluster state to sync
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::cout << "Replica node ID: " << cluster::get_local_node_id() << std::endl;

    SharedState* state = reinterpret_cast<SharedState*>(r->base_address());
    
    auto status = state->lock.lock();
    if (status == robust_lock_status::owner_died) {
        state->data = 100;
    }
    state->data = 42;
    
    std::cout << "Replica acquired lock. Simulating crash (infinite loop)..." << std::endl;
    // We hold the lock and never release it. The process will be killed.
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    return 0; // never reached
}

// -------------------------------------------------------------------------------------------------
// Main Test Case
// -------------------------------------------------------------------------------------------------

TEST_F(RobustLockTest, LockRecoveryAfterCrash) {
    const auto& args = testing::internal::GetArgvs();
    bool is_replica = false;
    uint16_t replica_port = 0;
    for (const auto& arg : args) {
        if (arg == "--run_as_robust_lock_replica") {
            is_replica = true;
        } else if (arg.find("--replica_port=") == 0) {
            replica_port = std::stoi(arg.substr(13));
        }
    }

    if (is_replica) {
        try {
            run_replica_that_grabs_lock(replica_port);
        } catch (const std::exception& e) {
            std::cerr << "Replica crashed with exception: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Replica crashed with unknown exception" << std::endl;
        }
        std::exit(1);
    }

    std::string exec_path = args[0];

    network_node->start_primary(port);

    SharedState* state = reinterpret_cast<SharedState*>(region->base_address());
    new (state) SharedState();
    state->data = 0;

    std::string cmd = "\"" + exec_path + "\" --gtest_filter=RobustLockTest.LockRecoveryAfterCrash --run_as_robust_lock_replica --replica_port=" + std::to_string(port);

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
        // Child process
        std::exit(run_replica_that_grabs_lock(port));
    }
#endif

    // Wait for the child to grab the lock and write 42
    auto start_wait_replica = std::chrono::steady_clock::now();
    while (state->data != 42) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start_wait_replica > std::chrono::seconds(5)) {
            break; // Timeout waiting for replica
        }
    }
    EXPECT_EQ(state->data, 42);

    // Assert that the lock is currently held
    // Try to acquire the lock, which should spin because the child holds it.
    std::atomic<bool> lock_acquired{false};
    std::atomic<bool> owner_died_flag{false};
    
    std::thread acquirer([&]() {
        auto status = state->lock.lock();
        if (status == robust_lock_status::owner_died) {
            owner_died_flag = true;
        }
        lock_acquired = true;
        state->lock.unlock();
    });

    // Let the acquirer thread get into the spin loop
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_FALSE(lock_acquired.load());

    // Kill the replica!
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

    // After the child is killed, the network node should notice the disconnect,
    // update the cluster state, and the acquirer thread should detect this
    // and forcefully break the lock.
    
    // Wait for recovery
    auto start_wait = std::chrono::steady_clock::now();
    bool recovered = false;
    while (std::chrono::steady_clock::now() - start_wait < std::chrono::seconds(5)) {
        if (lock_acquired.load()) {
            recovered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    acquirer.join();

    EXPECT_TRUE(recovered) << "Failed to recover the lock after replica crash";
    EXPECT_TRUE(owner_died_flag.load()) << "Expected lock acquisition to report owner_died";
    EXPECT_EQ(state->data, 42) << "Expected replica to have written to data before crashing";
}
