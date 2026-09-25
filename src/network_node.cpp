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

#include "geryon/network_node.hpp"
#include "geryon/memory_region.hpp"
#include "geryon/cluster_state.hpp"
#include <iostream>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace geryon {

NetworkNode::NetworkNode(MemoryRegion* region, bool initial_owner)
    : region_(region), is_primary_(initial_owner) {
    std::size_t num_pages = region_->size() / MemoryRegion::system_page_size();
    
    if (is_primary_) {
        primary_page_owner_.resize(num_pages, 0); // 0 = Primary owns it
        primary_page_waiters_.resize(num_pages);
        // Primary starts with all pages accessible.
        page_accessible_.resize(num_pages, true);
    } else {
        replica_page_owned_.resize(num_pages, false);
        replica_read_buffer_.resize(MemoryRegion::system_page_size());
        // Replica starts with no pages accessible.
        page_accessible_.resize(num_pages, false);
    }
    page_received_.resize(num_pages, false);
    page_request_in_flight_.resize(num_pages, false);
}

NetworkNode::~NetworkNode() {
    stop();
}

void NetworkNode::start_primary(uint16_t port) {
    if (!is_primary_) throw std::runtime_error("Cannot start replica as primary");
    
    cluster::internal::set_local_node_id(0);

    boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(), port);
    acceptor_ = std::make_unique<boost::asio::ip::tcp::acceptor>(io_context_);
    acceptor_->open(endpoint.protocol());
    acceptor_->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_->bind(endpoint);
    acceptor_->listen();
    
    accept_connection();
    
    io_thread_ = std::thread([this]() {
        try {
            io_context_.run();
        } catch (const std::exception& e) {
            std::cerr << "Primary IO thread exception: " << e.what() << std::endl;
        }
    });
}

void NetworkNode::start_replica(const std::string& host, uint16_t port, bool read_only) {
    if (is_primary_) throw std::runtime_error("Cannot start primary as replica");
    
    read_only_mode_ = read_only;
    is_connected_to_primary_ = true;

    replica_socket_ = std::make_shared<boost::asio::ip::tcp::socket>(io_context_);
    boost::asio::ip::tcp::resolver resolver(io_context_);
    auto endpoints = resolver.resolve(host, std::to_string(port));
    
    boost::asio::connect(*replica_socket_, endpoints);
    
    start_replica_read_loop();
    
    io_thread_ = std::thread([this]() {
        try {
            io_context_.run();
        } catch (const std::exception& e) {
            std::cerr << "Replica IO thread exception: " << e.what() << std::endl;
        }
    });
}

void NetworkNode::stop() {
    if (is_stopping_) return;
    is_stopping_ = true;

    if (is_primary_) {
        std::vector<uint32_t> client_ids;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (const auto& pair : clients_) {
                client_ids.push_back(pair.first);
            }
        }
        
        if (!client_ids.empty()) {
            pending_sync_responses_ = client_ids.size();
            for (uint32_t id : client_ids) {
                send_sync_request(id);
            }
            
            std::unique_lock<std::mutex> lock(wait_mutex_);
            sync_cv_.wait_for(lock, std::chrono::seconds(2), [this]() {
                return pending_sync_responses_ <= 0;
            });
        }
    } else {
        trigger_synchronization();
        
        // Wait for primary to acknowledge all data before closing
        // to prevent TCP RST on exit which can drop unread buffers
        pending_sync_responses_ = 1;
        boost::asio::post(io_context_, [this]() {
            send_sync_request(0);
        });
        
        std::unique_lock<std::mutex> lock(wait_mutex_);
        sync_cv_.wait_for(lock, std::chrono::seconds(2), [this]() {
            return pending_sync_responses_ <= 0;
        });

        if (replica_socket_ && replica_socket_->is_open()) {
            send_sync_response(0);
        }

        io_context_.stop();
        if (io_thread_.joinable()) {
            io_thread_.join();
        }

        if (replica_socket_ && replica_socket_->is_open()) {
            boost::system::error_code ec;
            replica_socket_->shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);
            replica_socket_->close(ec);
        }
    }

    if (is_primary_) {
        io_context_.stop();
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
        
        for (auto& [id, socket] : clients_) {
            if (socket && socket->is_open()) {
                boost::system::error_code ec;
                socket->close(ec);
            }
        }
    }

    if (acceptor_ && acceptor_->is_open()) {
        boost::system::error_code ec;
        acceptor_->close(ec);
    }
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
}

void NetworkNode::accept_connection() {
    auto new_socket = std::make_shared<boost::asio::ip::tcp::socket>(io_context_);
    acceptor_->async_accept(*new_socket, [this, new_socket](const boost::system::error_code& error) {
        if (!error) {
            uint32_t client_id = next_client_id_++;
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                clients_[client_id] = new_socket;
                client_write_mutexes_[client_id] = std::make_unique<std::mutex>();
            }
            primary_read_buffers_[client_id].resize(MemoryRegion::system_page_size());
            
            cluster::internal::add_active_node(client_id);
            
            std::vector<uint32_t> existing_clients;
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                for (const auto& pair : clients_) {
                    if (pair.first != client_id) {
                        existing_clients.push_back(pair.first);
                    }
                }
            }

            // Send Welcome to new client
            send_topology_message(client_id, MsgType::Welcome, client_id);

            // Announce Primary to new client
            send_topology_message(client_id, MsgType::NodeJoined, 0);

            // Send NodeJoined exchanges
            for (uint32_t other_id : existing_clients) {
                // Tell other client about the new client
                send_topology_message(other_id, MsgType::NodeJoined, client_id);
                // Tell the new client about the other client
                send_topology_message(client_id, MsgType::NodeJoined, other_id);
            }

            std::cout << "Replica " << client_id << " connected." << std::endl;
            start_primary_read_loop(client_id);
            
            accept_connection(); // Accept next
        } else if (error != boost::asio::error::operation_aborted) {
            std::cerr << "Accept failed: " << error.message() << std::endl;
        }
    });
}

// --------------------------------------------------------------------------------------
// REPLICA LOGIC
// --------------------------------------------------------------------------------------
void NetworkNode::start_replica_read_loop() {
    boost::asio::async_read(*replica_socket_,
        boost::asio::buffer(&replica_read_header_, sizeof(MsgHeader)),
        [this](const boost::system::error_code& error, std::size_t bytes) {
            handle_replica_read_header(error, bytes);
        });
}

void NetworkNode::handle_replica_read_header(const boost::system::error_code& error, std::size_t) {
    if (error) {
        if (error != boost::asio::error::operation_aborted && error != boost::asio::error::eof && error != boost::asio::error::connection_reset) {
            std::cerr << "Replica read header failed: " << error.message() << std::endl;
        }
        if (replica_socket_ && replica_socket_->is_open()) {
            boost::system::error_code ec;
            replica_socket_->close(ec);
        }
        handle_primary_disconnect();
        return;
    }

    uint32_t page_index = ntohl(replica_read_header_.page_index);

    if (replica_read_header_.type == MsgType::PageRequest) {
        bool should_send = false;
        {
            std::lock_guard<std::mutex> lock(ownership_mutex_);
            if (replica_page_owned_[page_index]) {
                replica_page_owned_[page_index] = false;
                should_send = true;
            }
        }

        if (should_send) {
            // Give up ownership and send to Primary
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            send_page_data(0, page_index);
        }
        start_replica_read_loop();
    } else if (replica_read_header_.type == MsgType::SyncRequest) {
        trigger_synchronization();
        send_sync_response(0);
        start_replica_read_loop();
    } else if (replica_read_header_.type == MsgType::SyncResponse) {
        pending_sync_responses_--;
        if (pending_sync_responses_ <= 0) {
            std::lock_guard<std::mutex> lock(wait_mutex_);
            sync_cv_.notify_all();
        }
        start_replica_read_loop();
    } else if (replica_read_header_.type == MsgType::Welcome) {
        cluster::internal::set_local_node_id(ntohl(replica_read_header_.node_id));
        start_replica_read_loop();
    } else if (replica_read_header_.type == MsgType::NodeJoined) {
        cluster::internal::add_active_node(ntohl(replica_read_header_.node_id));
        start_replica_read_loop();
    } else if (replica_read_header_.type == MsgType::NodeLeft) {
        cluster::internal::remove_active_node(ntohl(replica_read_header_.node_id));
        start_replica_read_loop();
    } else if (replica_read_header_.type == MsgType::PageData) {
        boost::asio::async_read(*replica_socket_,
            boost::asio::buffer(replica_read_buffer_.data(), replica_read_buffer_.size()),
            [this](const boost::system::error_code& e, std::size_t bytes) {
                handle_replica_read_data(e, bytes);
            });
    }
}

void NetworkNode::handle_replica_read_data(const boost::system::error_code& error, std::size_t) {
    if (error) return;

    uint32_t page_index = ntohl(replica_read_header_.page_index);
    std::size_t page_size = MemoryRegion::system_page_size();
    void* page_base = static_cast<char*>(region_->base_address()) + (page_index * page_size);
    void* io_base = static_cast<char*>(region_->io_address()) + (page_index * page_size);

    std::memcpy(io_base, replica_read_buffer_.data(), page_size);
    
    PageProtection prot = read_only_mode_ ? PageProtection::ReadOnly : PageProtection::ReadWrite;
    region_->set_protection(page_base, page_size, prot);

    bool should_bounce = false;
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        if (is_stopping_) {
            should_bounce = true;
        } else {
            replica_page_owned_[page_index] = true;
        }
    }

    if (should_bounce) {
        // Bounce the page back to the primary so it isn't lost when we exit
        auto header = std::make_shared<MsgHeader>();
        header->type = MsgType::PageData;
        header->page_index = htonl(page_index);
        
        auto data_buffer = std::make_shared<std::vector<uint8_t>>(replica_read_buffer_);
        
        std::array<boost::asio::const_buffer, 2> buffers = {
            boost::asio::buffer(header.get(), sizeof(MsgHeader)),
            boost::asio::buffer(data_buffer->data(), data_buffer->size())
        };
        
        boost::system::error_code ec;
        std::lock_guard<std::mutex> write_lock(replica_socket_write_mutex_);
        boost::asio::write(*replica_socket_, buffers, ec);
        
        start_replica_read_loop();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(wait_mutex_);
        page_received_[page_index] = true;
        page_request_in_flight_[page_index] = false;
        page_accessible_[page_index] = true;
        stats_.pages_received.fetch_add(1, std::memory_order_relaxed);
        wait_cv_.notify_all();
    }

    start_replica_read_loop();
}

// --------------------------------------------------------------------------------------
// PRIMARY LOGIC
// --------------------------------------------------------------------------------------
void NetworkNode::start_primary_read_loop(uint32_t client_id) {
    boost::asio::async_read(*clients_[client_id],
        boost::asio::buffer(&primary_read_headers_[client_id], sizeof(MsgHeader)),
        [this, client_id](const boost::system::error_code& error, std::size_t bytes) {
            handle_primary_read_header(client_id, error, bytes);
        });
}

void NetworkNode::handle_primary_read_header(uint32_t client_id, const boost::system::error_code& error, std::size_t) {
    if (error) {
        if (error != boost::asio::error::operation_aborted && error != boost::asio::error::eof && error != boost::asio::error::connection_reset) {
            std::cerr << "Primary read header failed for client " << client_id << ": " << error.message() << std::endl;
        }
        if (clients_.count(client_id)) {
            boost::system::error_code ec;
            clients_[client_id]->close(ec);
        }
        handle_client_disconnect(client_id);
        return;
    }

    uint32_t page_index = ntohl(primary_read_headers_[client_id].page_index);

    if (primary_read_headers_[client_id].type == MsgType::PageRequest) {
        // Replica requested a page
        uint32_t current_owner = 0;
        bool is_first_waiter = false;
        {
            std::lock_guard<std::mutex> lock(ownership_mutex_);
            is_first_waiter = primary_page_waiters_[page_index].empty();
            primary_page_waiters_[page_index].push(client_id);
            current_owner = primary_page_owner_[page_index];
        }

        if (is_first_waiter) {
            if (current_owner == 0) {
                // Primary currently owns it, send to replica
                // Delay slightly to prevent live-lock thrashing
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                send_page_data(client_id, page_index);
            } else {
                // Another replica owns it, request it
                send_page_request(current_owner, page_index);
            }
        }
        start_primary_read_loop(client_id);
    } else if (primary_read_headers_[client_id].type == MsgType::SyncResponse) {
        pending_sync_responses_--;
        if (pending_sync_responses_ <= 0) {
            std::lock_guard<std::mutex> lock(wait_mutex_);
            sync_cv_.notify_all();
        }
        start_primary_read_loop(client_id);
    } else if (primary_read_headers_[client_id].type == MsgType::SyncRequest) {
        send_sync_response(client_id);
        start_primary_read_loop(client_id);
    } else if (primary_read_headers_[client_id].type == MsgType::PageData) {
        // Replica returned a page
        boost::asio::async_read(*clients_[client_id],
            boost::asio::buffer(primary_read_buffers_[client_id].data(), primary_read_buffers_[client_id].size()),
            [this, client_id](const boost::system::error_code& e, std::size_t bytes) {
                handle_primary_read_data(client_id, e, bytes);
            });
    }
}

void NetworkNode::process_next_waiter(uint32_t page_index) {
    uint32_t next_target = 0;
    bool forward_needed = false;
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        if (!primary_page_waiters_[page_index].empty()) {
            next_target = primary_page_waiters_[page_index].front();
            primary_page_waiters_[page_index].pop();
            primary_page_owner_[page_index] = next_target;
            forward_needed = true;
        }
    }
    if (forward_needed) {
        boost::asio::post(io_context_, [this, next_target, page_index]() {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            send_page_data(next_target, page_index);
        });
    }
}

void NetworkNode::handle_primary_read_data(uint32_t client_id, const boost::system::error_code& error, std::size_t) {
    if (error) return;

    uint32_t page_index = ntohl(primary_read_headers_[client_id].page_index);
    std::size_t page_size = MemoryRegion::system_page_size();
    
    // Copy data locally temporarily
    std::vector<uint8_t> page_data_copy = primary_read_buffers_[client_id];

    uint32_t next_owner = 0;
    bool has_more_waiters = false;

    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        if (!primary_page_waiters_[page_index].empty()) {
            next_owner = primary_page_waiters_[page_index].front();
            primary_page_waiters_[page_index].pop();
        }
        primary_page_owner_[page_index] = next_owner;
        has_more_waiters = !primary_page_waiters_[page_index].empty();
    }

    if (next_owner == 0) {
        // Primary gets it
        void* page_base = static_cast<char*>(region_->base_address()) + (page_index * page_size);
        void* io_base = static_cast<char*>(region_->io_address()) + (page_index * page_size);
        
        std::memcpy(io_base, page_data_copy.data(), page_size);
        region_->set_protection(page_base, page_size, PageProtection::ReadWrite);

        {
            std::lock_guard<std::mutex> lock(wait_mutex_);
            page_received_[page_index] = true;
            page_request_in_flight_[page_index] = false;
            page_accessible_[page_index] = true;
            stats_.pages_received.fetch_add(1, std::memory_order_relaxed);
            wait_cv_.notify_all();
        }
        
        if (has_more_waiters) {
            process_next_waiter(page_index);
        }
        
        start_primary_read_loop(client_id);
        return;
    } else {
        // Forward data to the next owner replica!
        // We write directly to the socket from this IO thread.
        auto header = std::make_shared<MsgHeader>();
        header->type = MsgType::PageData;
        header->page_index = htonl(page_index);
        auto data_buffer = std::make_shared<std::vector<uint8_t>>(page_data_copy);

        std::array<boost::asio::const_buffer, 2> buffers = {
            boost::asio::buffer(header.get(), sizeof(MsgHeader)),
            boost::asio::buffer(data_buffer->data(), data_buffer->size())
        };

        {
            std::lock_guard<std::mutex> write_lock(*client_write_mutexes_[next_owner]);
            boost::system::error_code ec;
            boost::asio::write(*clients_[next_owner], buffers, ec);
        }

        // If there are MORE waiters, we must request it from the new owner!
        if (has_more_waiters) {
            send_page_request(next_owner, page_index);
        }
    }

    start_primary_read_loop(client_id);
}

// --------------------------------------------------------------------------------------
// SHARED FAULT HANDLING / REQUEST PAGE
// --------------------------------------------------------------------------------------
bool NetworkNode::request_page(void* fault_address) {
    char* base = static_cast<char*>(region_->base_address());
    char* addr = static_cast<char*>(fault_address);
    std::size_t page_size = MemoryRegion::system_page_size();
    
    if (addr < base || addr >= base + region_->size()) {
        return false; // Out of bounds
    }

    uint32_t page_index = (addr - base) / page_size;

    std::unique_lock<std::mutex> lock(wait_mutex_);
    bool originally_accessible = page_accessible_[page_index];

    while (true) {
        if (is_stopping_) return false;

        // Fast-path: another thread already made the page accessible for us.
        // This handles the case where two threads fault on the same page simultaneously
        // (common on macOS with SA_SIGINFO signal delivery).
        if (page_accessible_[page_index]) {
            if (originally_accessible && !is_primary_ && read_only_mode_ && is_connected_to_primary_) {
                // We have a fault on a page that is ALREADY accessible.
                // In read_only_mode, this MUST be a write fault because read faults wouldn't happen on a PROT_READ page!
                // Wait until we are no longer connected to the primary (i.e. it dies)
                wait_cv_.wait(lock, [this]() { return !is_connected_to_primary_ || is_stopping_; });
                if (is_stopping_) return false;
                
                // Primary is dead! We are promoted!
                region_->set_protection(base + (page_index * page_size), page_size, PageProtection::ReadWrite);
                return true;
            }

            PageProtection prot = (!is_primary_ && read_only_mode_ && is_connected_to_primary_) ? PageProtection::ReadOnly : PageProtection::ReadWrite;
            region_->set_protection(base + (page_index * page_size), page_size, prot);
            return true;
        }

        // Slow-path: page is not accessible; verify ownership and request if needed.
        bool owned = false;
        {
            std::lock_guard<std::mutex> own_lock(ownership_mutex_);
            if (is_primary_) {
                owned = (primary_page_owner_[page_index] == 0);
            } else {
                owned = replica_page_owned_[page_index];
            }
        }

        if (owned) {
            // We own the page but it's not yet marked accessible — make it so.
            PageProtection prot = (!is_primary_ && read_only_mode_ && is_connected_to_primary_) ? PageProtection::ReadOnly : PageProtection::ReadWrite;
            region_->set_protection(base + (page_index * page_size), page_size, prot);
            page_accessible_[page_index] = true;
            wait_cv_.notify_all(); // Wake any other threads waiting on this page.
            
            // If it was a write fault on read-only, loop will catch it on next iteration
            continue;
        }

        if (!is_primary_ && !is_connected_to_primary_) {
            // We are not connected to the primary and we don't own the page!
            // Recover by mapping locally to prevent crashing, but data is stale/lost
            region_->set_protection(base + (page_index * page_size), page_size, PageProtection::ReadWrite);
            page_accessible_[page_index] = true;
            {
                std::lock_guard<std::mutex> own_lock(ownership_mutex_);
                replica_page_owned_[page_index] = true;
            }
            wait_cv_.notify_all();
            return true;
        }

        // Issue a request if none is in flight yet.
        bool in_flight = page_request_in_flight_[page_index];
        if (!in_flight) {
            page_request_in_flight_[page_index] = true;
            // Release wait_mutex_ before sending to avoid potential deadlocks
            // (send_page_request acquires ownership_mutex_ internally).
            lock.unlock();
            if (!is_primary_) {
                send_page_request(0, page_index);
            } else {
                uint32_t current_owner = 0;
                {
                    std::lock_guard<std::mutex> own_lock(ownership_mutex_);
                    current_owner = primary_page_owner_[page_index];
                    primary_page_waiters_[page_index].push(0);
                }
                send_page_request(current_owner, page_index);
            }
            lock.lock();
        }

        wait_cv_.wait(lock, [this, page_index]() { 
            return page_accessible_[page_index] || is_stopping_ || (!is_primary_ && !is_connected_to_primary_); 
        });
    }
}

// --------------------------------------------------------------------------------------
// DATA TRANSMISSION
// --------------------------------------------------------------------------------------
void NetworkNode::send_page_request(uint32_t target_node_id, uint32_t page_index) {
    auto header = std::make_shared<MsgHeader>();
    header->type = MsgType::PageRequest;
    header->page_index = htonl(page_index);

    boost::system::error_code ec;
    
    if (!is_primary_) {
        std::lock_guard<std::mutex> lock(replica_socket_write_mutex_);
        boost::asio::write(*replica_socket_, boost::asio::buffer(header.get(), sizeof(MsgHeader)), ec);
    } else {
        std::mutex* client_mutex_ptr = nullptr;
        std::shared_ptr<boost::asio::ip::tcp::socket> socket_ptr;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            auto it = client_write_mutexes_.find(target_node_id);
            if (it != client_write_mutexes_.end()) {
                client_mutex_ptr = it->second.get();
                socket_ptr = clients_[target_node_id];
            }
        }
        if (client_mutex_ptr && socket_ptr) {
            std::lock_guard<std::mutex> lock(*client_mutex_ptr);
            boost::asio::write(*socket_ptr, boost::asio::buffer(header.get(), sizeof(MsgHeader)), ec);
        }
    }
    
    if (ec) {
        std::cerr << "Error sending page request: " << ec.message() << std::endl;
    }
}

void NetworkNode::send_page_data(uint32_t target_node_id, uint32_t page_index) {
    std::size_t page_size = MemoryRegion::system_page_size();
    void* page_base = static_cast<char*>(region_->base_address()) + (page_index * page_size);

    auto header = std::make_shared<MsgHeader>();
    header->type = MsgType::PageData;
    header->page_index = htonl(page_index);
    
    auto data_buffer = std::make_shared<std::vector<uint8_t>>(page_size);

    if (!is_primary_) {
        // REPLICA LOGIC: send to Primary
        std::lock_guard<std::mutex> write_lock(replica_socket_write_mutex_);

        // Ownership has already been relinquished by the caller.
        // Mark page as inaccessible so any concurrent faulting thread re-waits.
        {
            std::lock_guard<std::mutex> lock(wait_mutex_);
            page_accessible_[page_index] = false;
        }

        void* io_base = static_cast<char*>(region_->io_address()) + (page_index * page_size);

        region_->set_protection(page_base, page_size, PageProtection::None);
        std::memcpy(data_buffer->data(), io_base, page_size);

        std::array<boost::asio::const_buffer, 2> buffers = {
            boost::asio::buffer(header.get(), sizeof(MsgHeader)),
            boost::asio::buffer(data_buffer->data(), data_buffer->size())
        };

        boost::system::error_code ec;
        auto start_time = std::chrono::steady_clock::now();
        boost::asio::write(*replica_socket_, buffers, ec);
        auto end_time = std::chrono::steady_clock::now();
        
        uint64_t duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
        stats_.total_transfer_time_us.fetch_add(duration_us, std::memory_order_relaxed);
        
        {
            std::lock_guard<std::mutex> stat_lock(stats_mutex_);
            if (has_first_transfer_) {
                uint64_t between_us = std::chrono::duration_cast<std::chrono::microseconds>(start_time - last_transfer_end_time_).count();
                stats_.total_time_between_transfers_us.fetch_add(between_us, std::memory_order_relaxed);
            }
            has_first_transfer_ = true;
            last_transfer_end_time_ = end_time;
        }

        stats_.pages_sent.fetch_add(1, std::memory_order_relaxed);
    } else {
        // PRIMARY LOGIC: send to Replica
        std::mutex* client_mutex_ptr = nullptr;
        std::shared_ptr<boost::asio::ip::tcp::socket> socket_ptr;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            auto it = client_write_mutexes_.find(target_node_id);
            if (it != client_write_mutexes_.end()) {
                client_mutex_ptr = it->second.get();
                socket_ptr = clients_[target_node_id];
            }
        }
        if (!client_mutex_ptr || !socket_ptr) return;

        {
            std::lock_guard<std::mutex> write_lock(*client_mutex_ptr);

            {
                std::lock_guard<std::mutex> lock(ownership_mutex_);
                // Update ownership: pop the waiter and mark owner as the target.
                if (!primary_page_waiters_[page_index].empty() && primary_page_waiters_[page_index].front() == target_node_id) {
                    primary_page_owner_[page_index] = primary_page_waiters_[page_index].front();
                    primary_page_waiters_[page_index].pop();
                } else if (primary_page_owner_[page_index] == 0) {
                     primary_page_owner_[page_index] = target_node_id;
                }
            }

            // Mark page as inaccessible so any concurrent faulting thread re-waits.
            {
                std::lock_guard<std::mutex> lock(wait_mutex_);
                page_accessible_[page_index] = false;
            }

            void* io_base = static_cast<char*>(region_->io_address()) + (page_index * page_size);

            region_->set_protection(page_base, page_size, PageProtection::None);
            std::memcpy(data_buffer->data(), io_base, page_size);

            std::array<boost::asio::const_buffer, 2> buffers = {
                boost::asio::buffer(header.get(), sizeof(MsgHeader)),
                boost::asio::buffer(data_buffer->data(), data_buffer->size())
            };

            boost::system::error_code ec;
            auto start_time = std::chrono::steady_clock::now();
            boost::asio::write(*socket_ptr, buffers, ec);
            auto end_time = std::chrono::steady_clock::now();
            
            uint64_t duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
            stats_.total_transfer_time_us.fetch_add(duration_us, std::memory_order_relaxed);
            
            {
                std::lock_guard<std::mutex> stat_lock(stats_mutex_);
                if (has_first_transfer_) {
                    uint64_t between_us = std::chrono::duration_cast<std::chrono::microseconds>(start_time - last_transfer_end_time_).count();
                    stats_.total_time_between_transfers_us.fetch_add(between_us, std::memory_order_relaxed);
                }
                has_first_transfer_ = true;
                last_transfer_end_time_ = end_time;
            }

            stats_.pages_sent.fetch_add(1, std::memory_order_relaxed);
        }
        
        // If there are more waiters, we must request it!
        bool has_more_waiters = false;
        {
            std::lock_guard<std::mutex> lock(ownership_mutex_);
            has_more_waiters = !primary_page_waiters_[page_index].empty();
        }
        if (has_more_waiters) {
            send_page_request(target_node_id, page_index);
        }
    }
}

void NetworkNode::send_sync_request(uint32_t target_node_id) {
    auto header = std::make_shared<MsgHeader>();
    header->type = MsgType::SyncRequest;
    header->page_index = 0; 
    
    boost::system::error_code ec;
    if (!is_primary_) {
        std::lock_guard<std::mutex> lock(replica_socket_write_mutex_);
        boost::asio::write(*replica_socket_, boost::asio::buffer(header.get(), sizeof(MsgHeader)), ec);
    } else {
        std::lock_guard<std::mutex> lock(*client_write_mutexes_[target_node_id]);
        boost::asio::write(*clients_[target_node_id], boost::asio::buffer(header.get(), sizeof(MsgHeader)), ec);
    }
}

void NetworkNode::send_sync_response(uint32_t target_node_id) {
    auto header = std::make_shared<MsgHeader>();
    header->type = MsgType::SyncResponse;
    header->page_index = 0; 
    
    boost::system::error_code ec;
    if (!is_primary_) {
        std::lock_guard<std::mutex> lock(replica_socket_write_mutex_);
        boost::asio::write(*replica_socket_, boost::asio::buffer(header.get(), sizeof(MsgHeader)), ec);
    } else {
        std::lock_guard<std::mutex> lock(*client_write_mutexes_[target_node_id]);
        boost::asio::write(*clients_[target_node_id], boost::asio::buffer(header.get(), sizeof(MsgHeader)), ec);
    }
}

void NetworkNode::send_topology_message(uint32_t target_node_id, MsgType type, uint32_t subject_node_id) {
    auto header = std::make_shared<MsgHeader>();
    header->type = type;
    header->node_id = htonl(subject_node_id);
    
    boost::system::error_code ec;
    if (!is_primary_) {
        std::lock_guard<std::mutex> lock(replica_socket_write_mutex_);
        boost::asio::write(*replica_socket_, boost::asio::buffer(header.get(), sizeof(MsgHeader)), ec);
    } else {
        std::lock_guard<std::mutex> lock(*client_write_mutexes_[target_node_id]);
        boost::asio::write(*clients_[target_node_id], boost::asio::buffer(header.get(), sizeof(MsgHeader)), ec);
    }
}

void NetworkNode::trigger_synchronization() {
    std::size_t num_pages = region_->size() / MemoryRegion::system_page_size();
    if (is_primary_) {
        for (std::size_t i = 0; i < num_pages; ++i) {
            bool requires_pull = false;
            {
                std::lock_guard<std::mutex> lock(ownership_mutex_);
                requires_pull = (primary_page_owner_[i] != 0);
            }
            if (requires_pull) {
                void* page_addr = static_cast<char*>(region_->base_address()) + (i * MemoryRegion::system_page_size());
                request_page(page_addr);
            }
        }
    } else {
        for (std::size_t i = 0; i < num_pages; ++i) {
            bool should_send = false;
            {
                std::lock_guard<std::mutex> lock(ownership_mutex_);
                if (replica_page_owned_[i]) {
                    replica_page_owned_[i] = false;
                    should_send = true;
                }
            }
            if (should_send) {
                send_page_data(0, i);
            }
        }
    }
}

void NetworkNode::handle_primary_disconnect() {
    std::lock_guard<std::mutex> lock(wait_mutex_);
    if (!is_connected_to_primary_) return;
    is_connected_to_primary_ = false;
    cluster::internal::clear_active_nodes();
    
    if (read_only_mode_) {
        read_only_mode_ = false; // Promote to ReadWrite
        
        // Upgrade all currently accessible pages to ReadWrite
        std::size_t page_size = MemoryRegion::system_page_size();
        for (size_t i = 0; i < page_accessible_.size(); ++i) {
            if (page_accessible_[i]) {
                void* page_base = static_cast<char*>(region_->base_address()) + (i * page_size);
                region_->set_protection(page_base, page_size, PageProtection::ReadWrite);
            }
        }
    }
    
    wait_cv_.notify_all();

    if (on_primary_disconnect_) {
        on_primary_disconnect_();
    }
}

void NetworkNode::handle_client_disconnect(uint32_t client_id) {
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (const auto& pair : clients_) {
            if (pair.first != client_id) {
                // We use async writes via send_topology_message, but wait,
                // we shouldn't hold the mutex while doing write operations if possible,
                // but send_topology_message grabs its own lock.
            }
        }
    }
    
    // Announce to remaining clients
    std::vector<uint32_t> remaining_clients;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (const auto& pair : clients_) {
            if (pair.first != client_id) remaining_clients.push_back(pair.first);
        }
        clients_.erase(client_id);
    }
    
    // Update local cluster state tracking
    cluster::internal::remove_active_node(client_id);

    for (uint32_t other : remaining_clients) {
        send_topology_message(other, MsgType::NodeLeft, client_id);
    }
    
    std::lock_guard<std::mutex> lock(ownership_mutex_);
    for (size_t i = 0; i < primary_page_owner_.size(); ++i) {
        if (primary_page_owner_[i] == client_id) {
            primary_page_owner_[i] = 0;
            
            void* page_base = static_cast<char*>(region_->base_address()) + (i * MemoryRegion::system_page_size());
            region_->set_protection(page_base, MemoryRegion::system_page_size(), PageProtection::ReadWrite);
            
            {
                std::lock_guard<std::mutex> wait_lock(wait_mutex_);
                page_accessible_[i] = true;
            }
            wait_cv_.notify_all();
            
            process_next_waiter(i);
        }
    }
    
    for (size_t i = 0; i < primary_page_waiters_.size(); ++i) {
        std::queue<uint32_t> new_q;
        while (!primary_page_waiters_[i].empty()) {
            uint32_t w = primary_page_waiters_[i].front();
            primary_page_waiters_[i].pop();
            if (w != client_id) new_q.push(w);
        }
        primary_page_waiters_[i] = std::move(new_q);
    }
}

} // namespace geryon
