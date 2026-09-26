# Robust Distributed Synchronization

Based on your preference for **Option 1 (Node-ID Based Locks + Cluster State Table)**, here is the implementation plan:

### 1. Protocol & Topology Enhancements
*   Currently, the Primary assigns sequential `client_id`s internally, but replicas do not know their own IDs, nor do they know about each other.
*   We will introduce new protocol message types: `Welcome`, `NodeJoined`, and `NodeLeft`.
*   **Primary (Node 0)** will broadcast cluster topology changes to all connected replicas.
*   **Replicas** will maintain a local thread-safe registry of active node IDs.

### 2. Cluster State API
We will add a new globally accessible namespace `geryon::cluster`:
*   `uint32_t get_local_node_id()`: Returns `0` for Primary, or `>0` for Replicas.
*   `bool is_node_alive(uint32_t node_id)`: Checks the local registry to see if a node is currently connected to the cluster.

### 3. `geryon::robust_spin_lock`
We will introduce a new synchronization primitive specifically designed for distributed recovery:
*   Instead of a simple PID/thread hash, the atomic owner will store a 64-bit value: `(node_id << 32) | local_thread_id`.
*   The `lock()` method will spin. If blocked, it extracts the `node_id` from the lock's current owner and calls `is_node_alive(node_id)`.
*   If the owner node has disconnected (crashed, network drop, etc.), the waiting thread **forcefully breaks the lock** using atomic compare-and-swap.
*   The method will return a status enum: `success` (normal acquisition) or `owner_died` (the lock was recovered from a dead node, warning the caller that the protected data structure might be in an inconsistent state and requires repair, mirroring POSIX `EOWNERDEAD`).

This approach provides robust, dead-node recovery without requiring out-of-band lock managers or slow leases, keeping everything operating at memory-mapped speeds.
