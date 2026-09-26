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
#include <chrono>
#include "geryon/cluster_state.hpp"
#include "geryon/robust_spin_lock.hpp" // For robust_lock_status

namespace geryon {

/// A distributed lock that uses epoch/heartbeat counters.
/// The holding node MUST periodically call `heartbeat()` if it intends to hold
/// the lock for a long time. If a node fails to update its heartbeat within the
/// timeout window (or if it drops from the cluster), another node will forcefully
/// steal the lock.
class robust_epoch_lock {
public:
    robust_epoch_lock() noexcept : owner_node_(0), heartbeat_epoch_(0) {}

    /// Acquire the lock. If the current owner is dead or fails to update its
    /// heartbeat within `steal_timeout_ms`, the lock will be stolen.
    robust_lock_status lock(uint32_t steal_timeout_ms = 5000) noexcept {
        uint32_t my_node = cluster::get_local_node_id();
        
        while (true) {
            uint32_t expected = 0;
            if (owner_node_.compare_exchange_weak(expected, my_node, std::memory_order_acquire, std::memory_order_relaxed)) {
                heartbeat_epoch_.store(1, std::memory_order_release);
                return robust_lock_status::success;
            }

            if (expected == my_node) {
                // Not recursive, but if we somehow see ourselves, assume we have it.
                return robust_lock_status::success;
            }

            uint64_t initial_epoch = heartbeat_epoch_.load(std::memory_order_acquire);
            auto wait_start = std::chrono::steady_clock::now();

            while (owner_node_.load(std::memory_order_relaxed) == expected) {
                // If the owner dropped from the cluster, steal immediately.
                if (!cluster::is_node_alive(expected)) {
                    if (owner_node_.compare_exchange_strong(expected, my_node, std::memory_order_acquire, std::memory_order_relaxed)) {
                        heartbeat_epoch_.store(1, std::memory_order_release);
                        return robust_lock_status::owner_died;
                    }
                    break; // Restart
                }

                // Check heartbeat
                uint64_t current_epoch = heartbeat_epoch_.load(std::memory_order_acquire);
                if (current_epoch != initial_epoch) {
                    // Heartbeat updated! Reset timer.
                    initial_epoch = current_epoch;
                    wait_start = std::chrono::steady_clock::now();
                } else {
                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - wait_start).count() > steal_timeout_ms) {
                        // Heartbeat timed out! The node might be deadlocked or overloaded.
                        // Forcefully steal the lock.
                        if (owner_node_.compare_exchange_strong(expected, my_node, std::memory_order_acquire, std::memory_order_relaxed)) {
                            heartbeat_epoch_.store(1, std::memory_order_release);
                            return robust_lock_status::owner_died; // Treat as owner died
                        }
                        break; // Restart
                    }
                }

                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }
    }

    /// Update the heartbeat. Must be called periodically by the owner.
    void heartbeat() noexcept {
        uint32_t my_node = cluster::get_local_node_id();
        if (owner_node_.load(std::memory_order_relaxed) == my_node) {
            heartbeat_epoch_.fetch_add(1, std::memory_order_release);
        }
    }

    /// Unlock the mutex.
    void unlock() noexcept {
        uint32_t my_node = cluster::get_local_node_id();
        uint32_t expected = my_node;
        if (owner_node_.compare_exchange_strong(expected, 0, std::memory_order_release, std::memory_order_relaxed)) {
            heartbeat_epoch_.store(0, std::memory_order_relaxed);
        }
    }

private:
    std::atomic<uint32_t> owner_node_;
    std::atomic<uint64_t> heartbeat_epoch_;
};

} // namespace geryon
