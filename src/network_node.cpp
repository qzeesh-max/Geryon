#include "geryon/network_node.hpp"
#include "geryon/memory_region.hpp"
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
    } else {
        replica_page_owned_.resize(num_pages, false);
        replica_read_buffer_.resize(MemoryRegion::system_page_size());
    }
}

NetworkNode::~NetworkNode() {
    stop();
}

void NetworkNode::start_primary(uint16_t port) {
    if (!is_primary_) throw std::runtime_error("Cannot start replica as primary");
    
    boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(), port);
    acceptor_ = std::make_unique<boost::asio::ip::tcp::acceptor>(io_context_, endpoint);
    
    accept_connection();
    
    io_thread_ = std::thread([this]() {
        try {
            io_context_.run();
        } catch (const std::exception& e) {
            std::cerr << "Primary IO thread exception: " << e.what() << std::endl;
        }
    });
}

void NetworkNode::start_replica(const std::string& host, uint16_t port) {
    if (is_primary_) throw std::runtime_error("Cannot start primary as replica");
    
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
        for (const auto& [id, _] : clients_) {
            client_ids.push_back(id);
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
        if (replica_socket_ && replica_socket_->is_open()) {
            send_sync_response(0);
        }
    }

    io_context_.stop();
    if (replica_socket_ && replica_socket_->is_open()) {
        boost::system::error_code ec;
        replica_socket_->close(ec);
    }
    for (auto& [id, socket] : clients_) {
        if (socket && socket->is_open()) {
            boost::system::error_code ec;
            socket->close(ec);
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
            clients_[client_id] = new_socket;
            client_write_mutexes_[client_id] = std::make_unique<std::mutex>();
            primary_read_buffers_[client_id].resize(MemoryRegion::system_page_size());
            
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
        if (error != boost::asio::error::operation_aborted && error != boost::asio::error::eof) {
            std::cerr << "Replica read header failed: " << error.message() << std::endl;
        }
        return;
    }

    uint32_t page_index = ntohl(replica_read_header_.page_index);

    if (replica_read_header_.type == MsgType::PageRequest) {
        bool own_page = false;
        {
            std::lock_guard<std::mutex> lock(ownership_mutex_);
            own_page = replica_page_owned_[page_index];
        }

        if (own_page) {
            // Give up ownership and send to Primary
            send_page_data(0, page_index);
        }
        start_replica_read_loop();
    } else if (replica_read_header_.type == MsgType::SyncRequest) {
        trigger_synchronization();
        send_sync_response(0);
        start_replica_read_loop();
    } else if (replica_read_header_.type == MsgType::SyncResponse) {
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

    region_->set_protection(page_base, page_size, PageProtection::ReadWrite);
    std::memcpy(page_base, replica_read_buffer_.data(), page_size);
    
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        replica_page_owned_[page_index] = true;
    }

    {
        std::lock_guard<std::mutex> lock(wait_mutex_);
        if (waiting_for_page_ == page_index) {
            page_received_ = true;
            wait_cv_.notify_all();
        }
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
        if (error != boost::asio::error::operation_aborted && error != boost::asio::error::eof) {
            std::cerr << "Primary read header failed for client " << client_id << ": " << error.message() << std::endl;
        }
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
        region_->set_protection(page_base, page_size, PageProtection::ReadWrite);
        std::memcpy(page_base, page_data_copy.data(), page_size);

        {
            std::lock_guard<std::mutex> lock(wait_mutex_);
            if (waiting_for_page_ == page_index) {
                page_received_ = true;
                wait_cv_.notify_all();
            }
        }
        
        // If there are more waiters, primary must immediately send it to the next
        if (has_more_waiters) {
            uint32_t next_target;
            {
                std::lock_guard<std::mutex> lock(ownership_mutex_);
                next_target = primary_page_waiters_[page_index].front();
            }
            // Primary immediately sends it!
            send_page_data(next_target, page_index);
        }
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

    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        if (is_primary_) {
            if (primary_page_owner_[page_index] == 0) {
                region_->set_protection(base + (page_index * page_size), page_size, PageProtection::ReadWrite);
                return true;
            }
        } else {
            if (replica_page_owned_[page_index]) {
                region_->set_protection(base + (page_index * page_size), page_size, PageProtection::ReadWrite);
                return true;
            }
        }
    }

    std::unique_lock<std::mutex> lock(wait_mutex_);
    
    // Double check
    {
        std::lock_guard<std::mutex> own_lock(ownership_mutex_);
        if (is_primary_) {
            if (primary_page_owner_[page_index] == 0) {
                region_->set_protection(base + (page_index * page_size), page_size, PageProtection::ReadWrite);
                return true;
            }
        } else {
            if (replica_page_owned_[page_index]) {
                region_->set_protection(base + (page_index * page_size), page_size, PageProtection::ReadWrite);
                return true;
            }
        }
    }

    waiting_for_page_ = page_index;
    page_received_ = false;

    // Send request
    if (!is_primary_) {
        // Replica sends request to Primary (0)
        send_page_request(0, page_index);
    } else {
        // Primary queues itself. 
        bool is_first_waiter = false;
        uint32_t current_owner = 0;
        {
            std::lock_guard<std::mutex> own_lock(ownership_mutex_);
            is_first_waiter = primary_page_waiters_[page_index].empty();
            primary_page_waiters_[page_index].push(0);
            current_owner = primary_page_owner_[page_index];
        }
        
        if (is_first_waiter) {
            send_page_request(current_owner, page_index);
        }
    }

    wait_cv_.wait(lock, [this, page_index]() {
        return page_received_ && waiting_for_page_ == page_index;
    });

    waiting_for_page_ = 0xFFFFFFFF;
    return true;
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
        std::lock_guard<std::mutex> lock(*client_write_mutexes_[target_node_id]);
        boost::asio::write(*clients_[target_node_id], boost::asio::buffer(header.get(), sizeof(MsgHeader)), ec);
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

        {
            std::lock_guard<std::mutex> lock(ownership_mutex_);
            replica_page_owned_[page_index] = false;
        }

        region_->set_protection(page_base, page_size, PageProtection::ReadOnly);
        std::memcpy(data_buffer->data(), page_base, page_size);
        region_->set_protection(page_base, page_size, PageProtection::None);

        std::array<boost::asio::const_buffer, 2> buffers = {
            boost::asio::buffer(header.get(), sizeof(MsgHeader)),
            boost::asio::buffer(data_buffer->data(), data_buffer->size())
        };

        boost::system::error_code ec;
        boost::asio::write(*replica_socket_, buffers, ec);
    } else {
        // PRIMARY LOGIC: send to Replica
        {
            std::lock_guard<std::mutex> write_lock(*client_write_mutexes_[target_node_id]);

            {
                std::lock_guard<std::mutex> lock(ownership_mutex_);
                // Since Primary is sending from its own faulting or queue logic, update ownership
                // Wait, Primary ONLY calls send_page_data when IT owned the page initially (or just received it).
                // Actually, primary_page_owner_ is already updated by handle_primary_read_data before forwarding!
                // Wait, if Primary owned it and a Replica requested it:
                // handle_primary_read_header -> send_page_data.
                // We need to pop the waiter and set the owner!
                if (!primary_page_waiters_[page_index].empty() && primary_page_waiters_[page_index].front() == target_node_id) {
                    primary_page_owner_[page_index] = primary_page_waiters_[page_index].front();
                    primary_page_waiters_[page_index].pop();
                } else if (primary_page_owner_[page_index] == 0) {
                     primary_page_owner_[page_index] = target_node_id;
                }
            }

            region_->set_protection(page_base, page_size, PageProtection::ReadOnly);
            std::memcpy(data_buffer->data(), page_base, page_size);
            region_->set_protection(page_base, page_size, PageProtection::None);

            std::array<boost::asio::const_buffer, 2> buffers = {
                boost::asio::buffer(header.get(), sizeof(MsgHeader)),
                boost::asio::buffer(data_buffer->data(), data_buffer->size())
            };

            boost::system::error_code ec;
            boost::asio::write(*clients_[target_node_id], buffers, ec);
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
            bool is_owned = false;
            {
                std::lock_guard<std::mutex> lock(ownership_mutex_);
                is_owned = replica_page_owned_[i];
            }
            if (is_owned) {
                send_page_data(0, i);
            }
        }
    }
}

} // namespace geryon
