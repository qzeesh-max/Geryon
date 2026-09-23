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

#include "geryon/fault_handler.hpp"
#include <mutex>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#endif

namespace geryon {

struct RegionEntry {
    void* base_address;
    std::size_t size;
    PageFaultCallback callback;
};

class GlobalFaultRegistry {
public:
    static GlobalFaultRegistry& instance() {
        static GlobalFaultRegistry inst;
        return inst;
    }

    void register_region(void* base, std::size_t size, PageFaultCallback cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back({base, size, std::move(cb)});
        setup_handler_if_needed();
    }

    void unregister_region(void* base) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.erase(
            std::remove_if(entries_.begin(), entries_.end(),
                           [base](const RegionEntry& e) { return e.base_address == base; }),
            entries_.end());
    }

    bool handle_fault(void* address) {
        PageFaultCallback cb;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& entry : entries_) {
                char* addr = static_cast<char*>(address);
                char* base = static_cast<char*>(entry.base_address);
                if (addr >= base && addr < base + entry.size) {
                    cb = entry.callback;
                    break;
                }
            }
        }
        if (cb) {
            return cb(address);
        }
        return false;
    }

private:
    GlobalFaultRegistry() = default;

    void setup_handler_if_needed();

    std::mutex mutex_;
    std::vector<RegionEntry> entries_;
    bool handler_installed_{false};

#ifndef _WIN32
    struct sigaction old_segv_action_;
    struct sigaction old_bus_action_;
    static void sig_handler(int sig, siginfo_t* si, void* unused);
#else
    PVOID veh_handle_{nullptr};
    static LONG WINAPI vectored_handler(EXCEPTION_POINTERS* ep);
#endif
};

#ifndef _WIN32
void GlobalFaultRegistry::setup_handler_if_needed() {
    if (handler_installed_) return;
    struct sigaction sa;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = &GlobalFaultRegistry::sig_handler;
    if (sigaction(SIGSEGV, &sa, &old_segv_action_) == -1) {
        // Handle error
    }
    if (sigaction(SIGBUS, &sa, &old_bus_action_) == -1) {
        // Handle error
    }
    handler_installed_ = true;
}

void GlobalFaultRegistry::sig_handler(int sig, siginfo_t* si, void* context) {
    if (sig == SIGSEGV || sig == SIGBUS) {
        void* fault_addr = si->si_addr;
        if (GlobalFaultRegistry::instance().handle_fault(fault_addr)) {
            return; // Handled successfully, resume execution
        }
    }
    
    // If not handled, call previous handler or abort
    struct sigaction& old = (sig == SIGSEGV) ? GlobalFaultRegistry::instance().old_segv_action_ 
                                             : GlobalFaultRegistry::instance().old_bus_action_;
    if (old.sa_flags & SA_SIGINFO) {
        if (old.sa_sigaction) old.sa_sigaction(sig, si, context);
    } else {
        if (old.sa_handler == SIG_DFL) {
            signal(sig, SIG_DFL);
            raise(sig);
        } else if (old.sa_handler == SIG_IGN) {
            // Ignored
        } else {
            old.sa_handler(sig);
        }
    }
}
#else
void GlobalFaultRegistry::setup_handler_if_needed() {
    if (handler_installed_) return;
    veh_handle_ = AddVectoredExceptionHandler(1, &GlobalFaultRegistry::vectored_handler);
    handler_installed_ = true;
}

LONG WINAPI GlobalFaultRegistry::vectored_handler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        void* fault_addr = (void*)ep->ExceptionRecord->ExceptionInformation[1];
        if (GlobalFaultRegistry::instance().handle_fault(fault_addr)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// The actual OS-specific implementation wrappers
#ifndef _WIN32
class FaultHandlerPosix : public FaultHandler {
public:
    void register_region(void* base_address, std::size_t size, PageFaultCallback callback) override {
        GlobalFaultRegistry::instance().register_region(base_address, size, std::move(callback));
    }
    void unregister_region(void* base_address) override {
        GlobalFaultRegistry::instance().unregister_region(base_address);
    }
};

std::unique_ptr<FaultHandler> FaultHandler::create() {
    return std::make_unique<FaultHandlerPosix>();
}
#else
class FaultHandlerWindows : public FaultHandler {
public:
    void register_region(void* base_address, std::size_t size, PageFaultCallback callback) override {
        GlobalFaultRegistry::instance().register_region(base_address, size, std::move(callback));
    }
    void unregister_region(void* base_address) override {
        GlobalFaultRegistry::instance().unregister_region(base_address);
    }
};

std::unique_ptr<FaultHandler> FaultHandler::create() {
    return std::make_unique<FaultHandlerWindows>();
}
#endif

} // namespace geryon
