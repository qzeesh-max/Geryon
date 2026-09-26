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

/// A robust ticket-based spin lock providing strict FIFO fairness.
/// If a node dies while waiting or holding the lock, it is automatically
/// skipped by other waiting nodes to ensure the system does not stall.
class robust_ticket_lock {
public:
    robust_ticket_lock() noexcept {
        for (int i = 0; i < MAX_TICKETS; ++i) {
            ticket_owners_[i].store(INVALID_NODE_ID, std::memory_order_relaxed);
        }
    }

    robust_lock_status lock() noexcept {
        uint32_t my_node = cluster::get_local_node_id();
        uint64_t my_ticket = next_ticket_.fetch_add(1, std::memory_order_relaxed);
        
        // Register ourselves as the owner of this ticket.
        ticket_owners_[my_ticket % MAX_TICKETS].store(my_node, std::memory_order_release);

        auto wait_start = std::chrono::steady_clock::now();
        uint64_t last_current = now_serving_.load(std::memory_order_acquire);

        while (true) {
            uint64_t current = now_serving_.load(std::memory_order_acquire);
            if (current == my_ticket) {
                return robust_lock_status::success;
            }
            if (current > my_ticket) {
                // We got skipped! Someone assumed we were dead (e.g. extreme network lag).
                // Re-acquire a new ticket to maintain liveness.
                return lock();
            }

            if (current != last_current) {
                wait_start = std::chrono::steady_clock::now();
                last_current = current;
            }

            uint32_t owner_of_current = ticket_owners_[current % MAX_TICKETS].load(std::memory_order_acquire);
            
            bool should_skip = false;
            if (owner_of_current != INVALID_NODE_ID) {
                if (!cluster::is_node_alive(owner_of_current)) {
                    should_skip = true;
                }
            } else {
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - wait_start).count() > 1000) {
                    // It has been 1 second and the owner hasn't registered their ID.
                    // Assume they died immediately after fetch_add.
                    should_skip = true;
                }
            }

            if (should_skip) {
                if (now_serving_.compare_exchange_strong(current, current + 1, std::memory_order_release)) {
                    // Clear the owner to avoid ABA issues if the ticket wraps modulo MAX_TICKETS.
                    ticket_owners_[current % MAX_TICKETS].store(INVALID_NODE_ID, std::memory_order_relaxed);
                }
            } else {
                // Sleep slightly to avoid busy-waiting entirely
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }
    }

    void unlock() noexcept {
        uint64_t current = now_serving_.load(std::memory_order_relaxed);
        // Clear our ownership before advancing the ticket
        ticket_owners_[current % MAX_TICKETS].store(INVALID_NODE_ID, std::memory_order_relaxed);
        now_serving_.fetch_add(1, std::memory_order_release);
    }

private:
    static constexpr uint32_t MAX_TICKETS = 1024;
    static constexpr uint32_t INVALID_NODE_ID = 0xFFFFFFFF;
    
    std::atomic<uint64_t> now_serving_{0};
    std::atomic<uint64_t> next_ticket_{0};
    std::atomic<uint32_t> ticket_owners_[MAX_TICKETS];
};

} // namespace geryon
