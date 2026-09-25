/*
 * Geryon - A Distributed Shared Memory Framework
 * Copyright (C) 2026 Zeeshan Qazi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "geryon/cluster_state.hpp"
#include <mutex>
#include <unordered_set>
#include <atomic>

namespace geryon {
namespace cluster {

namespace {
    std::atomic<uint32_t> g_local_node_id{0xFFFFFFFF};
    
    // We use a global mutex and unordered_set. Lock contention here is minimal
    // since this is only queried when a spin lock takes the slow path and
    // discovers it is blocked, which only happens on high contention.
    std::mutex g_active_nodes_mtx;
    std::unordered_set<uint32_t> g_active_nodes;
}

uint32_t get_local_node_id() noexcept {
    return g_local_node_id.load(std::memory_order_relaxed);
}

bool is_node_alive(uint32_t node_id) noexcept {
    // A node is trivially alive if it is itself
    if (node_id == get_local_node_id()) {
        return true;
    }
    std::lock_guard<std::mutex> lock(g_active_nodes_mtx);
    return g_active_nodes.find(node_id) != g_active_nodes.end();
}

namespace internal {

void set_local_node_id(uint32_t node_id) noexcept {
    g_local_node_id.store(node_id, std::memory_order_release);
    // Automatically consider ourselves alive
    add_active_node(node_id);
}

void add_active_node(uint32_t node_id) noexcept {
    std::lock_guard<std::mutex> lock(g_active_nodes_mtx);
    g_active_nodes.insert(node_id);
}

void remove_active_node(uint32_t node_id) noexcept {
    std::lock_guard<std::mutex> lock(g_active_nodes_mtx);
    g_active_nodes.erase(node_id);
}

void clear_active_nodes() noexcept {
    std::lock_guard<std::mutex> lock(g_active_nodes_mtx);
    g_active_nodes.clear();
}

} // namespace internal
} // namespace cluster
} // namespace geryon
