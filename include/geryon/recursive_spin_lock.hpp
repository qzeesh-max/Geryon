#pragma once

#include <atomic>
#include <thread>

namespace geryon {

class recursive_spin_lock {
public:
    recursive_spin_lock() noexcept : owner_(0), recursion_count_(0) {}

    void lock() noexcept {
        auto current_thread_id = std::this_thread::get_id();
        auto current_id_hash = std::hash<std::thread::id>{}(current_thread_id);

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
        auto current_thread_id = std::this_thread::get_id();
        auto current_id_hash = std::hash<std::thread::id>{}(current_thread_id);

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
