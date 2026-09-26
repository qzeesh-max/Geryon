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

// Common definitions
#include "geryon/cluster_state.hpp"

// Distributed Spin Locks
#include "geryon/robust_spin_lock.hpp"
#include "geryon/robust_ticket_lock.hpp"
#include "geryon/robust_epoch_lock.hpp"
#include "geryon/robust_mcs_lock.hpp"

namespace geryon {
namespace sync {

// Aliases for convenience
using spin_lock   = robust_spin_lock;
using ticket_lock = robust_ticket_lock;
using epoch_lock  = robust_epoch_lock;
using mcs_lock    = robust_mcs_lock;

} // namespace sync
} // namespace geryon
