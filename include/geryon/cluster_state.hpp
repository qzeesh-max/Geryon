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
#include <cstdint>

namespace geryon {
namespace cluster {

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

/// Returns the node ID of the current process within the Geryon cluster.
/// - Node 0 is always the Primary Broker.
/// - Nodes > 0 are connected Replicas.
/// - Returns 0xFFFFFFFF if networking has not yet started.
uint32_t get_local_node_id() noexcept;

/// Returns true if the specified node ID is currently connected to the cluster.
/// The Primary tracks all Replicas, and Replicas track each other via Primary broadcasts.
bool is_node_alive(uint32_t node_id) noexcept;


// ----------------------------------------------------------------------------
// Internal API (Used by NetworkNode)
// ----------------------------------------------------------------------------
namespace internal {

void set_local_node_id(uint32_t node_id) noexcept;
void add_active_node(uint32_t node_id) noexcept;
void remove_active_node(uint32_t node_id) noexcept;
void clear_active_nodes() noexcept;

} // namespace internal
} // namespace cluster
} // namespace geryon
