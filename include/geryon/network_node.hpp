#pragma once
#include <boost/asio.hpp>
#include <string>
#include <memory>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <unordered_map>
#include <queue>

namespace geryon {

class MemoryRegion;

class NetworkNode {
public:
    // Initialize the network node for a specific memory region.
    // initial_owner should be true for the Primary (starts with all pages)
    // and false for the Replica (starts with no pages).
    NetworkNode(MemoryRegion* region, bool initial_owner);
    ~NetworkNode();

    // Start as Primary (listen for connection)
    void start_primary(uint16_t port);

    // Start as Replica (connect to Primary)
    void start_replica(const std::string& host, uint16_t port);

    // Stop networking
    void stop();

    // Invoked by the fault handler.
    // Blocks the thread until the page is fetched from the network.
    bool request_page(void* fault_address);

    // Forces all pages back to the Primary and ensures consistency
    void trigger_synchronization();

private:
    void accept_connection();
    
    // Replicas use this:
    void start_replica_read_loop();
    void handle_replica_read_header(const boost::system::error_code& error, std::size_t bytes_transferred);
    void handle_replica_read_data(const boost::system::error_code& error, std::size_t bytes_transferred);

    // Primary uses this:
    void start_primary_read_loop(uint32_t client_id);
    void handle_primary_read_header(uint32_t client_id, const boost::system::error_code& error, std::size_t bytes_transferred);
    void handle_primary_read_data(uint32_t client_id, const boost::system::error_code& error, std::size_t bytes_transferred);
    
    void send_page_request(uint32_t target_node_id, uint32_t page_index);
    void send_page_data(uint32_t target_node_id, uint32_t page_index);
    void send_sync_request(uint32_t target_node_id);
    void send_sync_response(uint32_t target_node_id);

    MemoryRegion* region_;
    bool is_primary_;
    
    boost::asio::io_context io_context_;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    
    // Replica socket
    std::shared_ptr<boost::asio::ip::tcp::socket> replica_socket_;
    
    // Primary sockets
    uint32_t next_client_id_{1};
    std::unordered_map<uint32_t, std::shared_ptr<boost::asio::ip::tcp::socket>> clients_;
    std::unordered_map<uint32_t, std::unique_ptr<std::mutex>> client_write_mutexes_;

    std::thread io_thread_;

    // Protocol state
    enum class MsgType : uint8_t {
        PageRequest = 1,
        PageData = 2,
        SyncRequest = 3,
        SyncResponse = 4
    };
    
    struct __attribute__((packed)) MsgHeader {
        MsgType type;
        uint32_t page_index;
    };

    // Replica read buffers
    MsgHeader replica_read_header_;
    std::vector<uint8_t> replica_read_buffer_;
    
    // Primary read buffers per client
    std::unordered_map<uint32_t, MsgHeader> primary_read_headers_;
    std::unordered_map<uint32_t, std::vector<uint8_t>> primary_read_buffers_;

    // Fault handling sync (Both Primary and Replica)
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    // Single-page wait variables (DEPRECATED, replace with vectors)
    std::vector<bool> page_received_;
    std::vector<bool> page_request_in_flight_;
    
    // Page ownership tracking
    // For Replica: true if it owns the page
    std::vector<bool> replica_page_owned_;
    
    // For Primary: 0 if Primary owns it, >0 if Replica ID owns it.
    std::vector<uint32_t> primary_page_owner_;
    
    // For Primary: queue of node IDs waiting for a page
    std::vector<std::queue<uint32_t>> primary_page_waiters_;
    
    std::mutex ownership_mutex_;
    
    // Socket synchronization for Replica
    std::mutex replica_socket_write_mutex_;

    // Consistency sync logic
    std::condition_variable sync_cv_;
    std::atomic<int> pending_sync_responses_{0};
    std::atomic<bool> is_stopping_{false};
};

} // namespace geryon
