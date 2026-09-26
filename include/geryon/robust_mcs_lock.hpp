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
#include "geryon/robust_spin_lock.hpp"

namespace geryon {

/// A Queue-Based Spin Lock (MCS Lock) that scales exceptionally well in DSM
/// because each node spins on its own memory location, avoiding cache coherence
/// storms on a single global atomic variable.
class robust_mcs_lock {
public:
    robust_mcs_lock() noexcept : tail_node_id_(INVALID_NODE_ID) {
        for (int i = 0; i < MAX_NODES; ++i) {
            nodes_[i].next_node_id.store(INVALID_NODE_ID, std::memory_order_relaxed);
            nodes_[i].ready.store(0, std::memory_order_relaxed);
        }
    }

    robust_lock_status lock() noexcept {
        uint32_t my_node = cluster::get_local_node_id();
        
        nodes_[my_node % MAX_NODES].next_node_id.store(INVALID_NODE_ID, std::memory_order_relaxed);
        nodes_[my_node % MAX_NODES].ready.store(0, std::memory_order_relaxed);

        uint32_t pred = tail_node_id_.exchange(my_node, std::memory_order_acq_rel);
        
        if (pred != INVALID_NODE_ID) {
            nodes_[pred % MAX_NODES].next_node_id.store(my_node, std::memory_order_release);
            
            while (nodes_[my_node % MAX_NODES].ready.load(std::memory_order_acquire) == 0) {
                if (pred != INVALID_NODE_ID && !cluster::is_node_alive(pred)) {
                    // Predecessor died!
                    // We must find who was pointing to pred and redirect it to us.
                    bool pred_was_owner = true;

                    for (uint32_t i = 0; i < MAX_NODES; ++i) {
                        uint32_t expected_next = pred;
                        if (nodes_[i].next_node_id.compare_exchange_strong(expected_next, my_node, std::memory_order_release)) {
                            pred_was_owner = false;
                            pred = i; // Our new predecessor is i
                            break;
                        }
                    }
                    
                    if (pred_was_owner) {
                        // Pred was the actual lock holder when it died!
                        return robust_lock_status::owner_died;
                    }
                }
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }
        return robust_lock_status::success;
    }

    void unlock() noexcept {
        uint32_t my_node = cluster::get_local_node_id();
        
        uint32_t next = nodes_[my_node % MAX_NODES].next_node_id.load(std::memory_order_acquire);
        if (next == INVALID_NODE_ID) {
            uint32_t expected = my_node;
            if (tail_node_id_.compare_exchange_strong(expected, INVALID_NODE_ID, std::memory_order_release, std::memory_order_relaxed)) {
                return;
            }
            // Someone appended themselves but hasn't updated our next_node_id yet!
            while ((next = nodes_[my_node % MAX_NODES].next_node_id.load(std::memory_order_acquire)) == INVALID_NODE_ID) {
                uint32_t current_tail = tail_node_id_.load(std::memory_order_acquire);
                if (current_tail != INVALID_NODE_ID && !cluster::is_node_alive(current_tail)) {
                    // The node appending to us died. Cut them off.
                    if (tail_node_id_.compare_exchange_strong(current_tail, INVALID_NODE_ID, std::memory_order_release)) {
                        return;
                    }
                }
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
        }
        
        nodes_[next % MAX_NODES].ready.store(1, std::memory_order_release);
    }

private:
    static constexpr uint32_t MAX_NODES = 256;
    static constexpr uint32_t INVALID_NODE_ID = 0xFFFFFFFF;

    struct alignas(64) mcs_node {
        std::atomic<uint32_t> next_node_id;
        std::atomic<uint32_t> ready;
    };

    std::atomic<uint32_t> tail_node_id_;
    mcs_node nodes_[MAX_NODES];
};

} // namespace geryon
