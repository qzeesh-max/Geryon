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

#pragma once

#include <atomic>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace geryon {

class recursive_spin_lock {
public:
    recursive_spin_lock() noexcept : owner_(0), recursion_count_(0) {}

    static std::size_t get_current_owner_id() noexcept {
        auto current_thread_id = std::this_thread::get_id();
        std::size_t thread_hash = std::hash<std::thread::id>{}(current_thread_id);
#ifdef _WIN32
        std::size_t pid = GetCurrentProcessId();
#else
        std::size_t pid = getpid();
#endif
        // Mix PID with thread hash. Left shift PID to affect higher bits, xor to combine.
        // This ensures the owner ID is unique across different processes.
        return thread_hash ^ (pid << 16) ^ (pid << 32);
    }

    void lock() noexcept {
        std::size_t current_id_hash = get_current_owner_id();

        if (owner_.load(std::memory_order_relaxed) == current_id_hash) {
            ++recursion_count_;
            return;
        }

        while (true) {
            std::size_t expected = 0;
            if (owner_.compare_exchange_weak(expected, current_id_hash, std::memory_order_acquire, std::memory_order_relaxed)) {
                recursion_count_ = 1;
                return;
            }
            // Sleep slightly to avoid busy-waiting entirely and allow other threads to progress,
            // also giving the network node a chance to process the page transfer.
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }

    void unlock() noexcept {
        std::size_t current_id_hash = get_current_owner_id();

        if (owner_.load(std::memory_order_relaxed) != current_id_hash) {
            // Cannot unlock if we do not own the lock
            return;
        }

        if (--recursion_count_ == 0) {
            owner_.store(0, std::memory_order_release);
        }
    }

private:
    std::atomic<std::size_t> owner_;
    std::size_t recursion_count_;
};

} // namespace geryon
