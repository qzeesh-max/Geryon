/*
 * Geryon - A Distributed Shared Memory Framework
 * Copyright (C) 2026 Zeeshan Qazi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once

#include <atomic>
#include <thread>
#include <cstdint>
#include "geryon/cluster_state.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace geryon {

enum class robust_lock_status {
    success,
    owner_died
};

/// A distributed spin lock that can automatically recover if the owning node
/// drops from the Geryon cluster.
class robust_spin_lock {
public:
    robust_spin_lock() noexcept : owner_(0), recursion_count_(0) {}

    static uint64_t get_current_owner_id() noexcept {
        auto current_thread_id = std::this_thread::get_id();
        uint32_t thread_hash = static_cast<uint32_t>(std::hash<std::thread::id>{}(current_thread_id));
        
#ifdef _WIN32
        uint32_t pid = GetCurrentProcessId();
#else
        uint32_t pid = getpid();
#endif
        // Combine PID and thread_hash to get a unique local identifier
        uint32_t local_id = thread_hash ^ pid;

        // Top 32-bits are the node ID. Bottom 32-bits are the local thread ID.
        uint64_t node_id = static_cast<uint64_t>(cluster::get_local_node_id());
        return (node_id << 32) | local_id;
    }

    robust_lock_status lock() noexcept {
        uint64_t current_id = get_current_owner_id();

        if (owner_.load(std::memory_order_relaxed) == current_id) {
            ++recursion_count_;
            return robust_lock_status::success;
        }

        while (true) {
            uint64_t expected = 0;
            if (owner_.compare_exchange_weak(expected, current_id, std::memory_order_acquire, std::memory_order_relaxed)) {
                recursion_count_ = 1;
                return robust_lock_status::success;
            }

            // We didn't get the lock. See if the current owner is still alive.
            uint64_t current_owner = expected;
            if (current_owner != 0) {
                uint32_t owner_node_id = static_cast<uint32_t>(current_owner >> 32);
                
                if (!cluster::is_node_alive(owner_node_id)) {
                    printf("robust_spin_lock: breaking lock held by dead node %u\n", owner_node_id);
                    // The node holding the lock has dropped from the cluster!
                    // Try to forcefully seize the lock.
                    if (owner_.compare_exchange_strong(current_owner, current_id, std::memory_order_acquire, std::memory_order_relaxed)) {
                        recursion_count_ = 1;
                        return robust_lock_status::owner_died;
                    }
                }
            }

            // Sleep slightly to avoid busy-waiting entirely and allow other threads to progress,
            // also giving the network node a chance to process the page transfer.
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }

    void unlock() noexcept {
        uint64_t current_id = get_current_owner_id();

        if (owner_.load(std::memory_order_relaxed) != current_id) {
            // Cannot unlock if we do not own the lock
            return;
        }

        if (--recursion_count_ == 0) {
            owner_.store(0, std::memory_order_release);
        }
    }

private:
    std::atomic<uint64_t> owner_;
    std::size_t recursion_count_;
};

} // namespace geryon
