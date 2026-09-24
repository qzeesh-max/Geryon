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
#include "geryon/memory_region.hpp"
#include <mutex>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <iostream>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#elif defined(__linux__)
// --------------------------------------------------------------------------
// Linux — sigaction(SIGSEGV) + self-pipe + per-region semaphore.
//
// The classic async-signal-safety problem: signal handlers must not call
// functions that are not async-signal-safe (e.g. malloc, mutex, cv, I/O).
// Our page fault callback calls std::mutex and network I/O, which are NOT
// async-signal-safe.
//
// Solution (self-pipe + semaphore pattern):
//
//   Signal handler (runs in faulting thread's context):
//     1. Finds the per-region semaphore for this fault address
//        (via a lock-free lookup in a static array)
//     2. Writes the fault address to the handler thread's pipe (write(2) is
//        async-signal-safe for O_NONBLOCK pipes with small payloads)
//     3. Calls sem_wait() to block until the handler thread completes
//        (sem_wait is async-signal-safe per POSIX)
//
//   Worker thread (runs in dedicated handler thread):
//     1. Reads fault address from pipe
//     2. Looks up and calls the full callback (which can freely use mutexes,
//        condition variables, network I/O — it's a normal thread context)
//     3. Calls sem_post() to wake the faulting thread
//
// This gives us the full power of userfaultfd-style deferred processing
// without requiring MAP_PRIVATE|MAP_ANONYMOUS or kernel 5.13+.
// --------------------------------------------------------------------------
#  include <sys/mman.h>
#  include <sys/syscall.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <semaphore.h>
#  include <string.h>
#  include <signal.h>
#  include <errno.h>
#  include <thread>
#  include <atomic>
#  include <cstring>
#  include <pthread.h>
#else
// macOS / other POSIX: use sigaction(SIGSEGV)
#  include <signal.h>
#endif

namespace geryon {

// ============================================================================
// Windows — Vectored Exception Handler
// ============================================================================
#ifdef _WIN32

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
        if (!handler_installed_) {
            veh_handle_ = AddVectoredExceptionHandler(1, &GlobalFaultRegistry::vectored_handler);
            handler_installed_ = true;
        }
    }

    void unregister_region(void* base) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.erase(
            std::remove_if(entries_.begin(), entries_.end(),
                           [base](const RegionEntry& e) { return e.base_address == base; }),
            entries_.end());
    }

private:
    GlobalFaultRegistry() = default;

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
        return cb ? cb(address) : false;
    }

    static LONG WINAPI vectored_handler(EXCEPTION_POINTERS* ep) {
        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            void* fault_addr = (void*)ep->ExceptionRecord->ExceptionInformation[1];
            if (GlobalFaultRegistry::instance().handle_fault(fault_addr)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    std::mutex mutex_;
    std::vector<RegionEntry> entries_;
    bool handler_installed_{false};
    PVOID veh_handle_{nullptr};
};

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

// ============================================================================
// Linux — sigaction(SIGSEGV) + self-pipe + semaphore
// ============================================================================
#elif defined(__linux__)

// Global registry entry for a registered memory region.
struct LinuxRegionEntry {
    void*             base;
    std::size_t       size;
    PageFaultCallback callback;
};

// ---------------------------------------------------------------------------
// GlobalFaultHandler — singleton that owns the pipe and worker thread.
// ---------------------------------------------------------------------------
class GlobalFaultHandler {
public:
    static GlobalFaultHandler& instance() {
        // Leak-on-exit singleton so it outlives any other static destructors.
        static GlobalFaultHandler* inst = new GlobalFaultHandler();
        return *inst;
    }

    // Register a region.  Must be called from a normal (non-signal) thread.
    void register_region(void* base, std::size_t size, PageFaultCallback cb) {
        auto entry = std::make_unique<LinuxRegionEntry>();
        entry->base = base;
        entry->size = size;
        entry->callback = std::move(cb);

        {
            std::lock_guard<std::mutex> lk(mutex_);
            regions_.push_back(std::move(entry));
            rebuild_snapshot();
        }

        install_handler_once();
    }

    void unregister_region(void* base) {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto it = regions_.begin(); it != regions_.end(); ++it) {
            if ((*it)->base == base) {
                regions_.erase(it);
                rebuild_snapshot();
                return;
            }
        }
    }

    // Called from the signal handler (async-signal context).
    // Writes the fault address to the pipe and then waits on the per-region
    // semaphore until the worker thread has handled the fault.
    // Returns true if the fault was handled, false if it should propagate.
    bool handle_fault_from_signal(void* addr) {
        // Lock-free region lookup via the regions_ snapshot.
        // We use a spin approach: iterate the raw pointer snapshot.
        // In the signal handler we can't lock a mutex.  Instead we read a
        // snapshot pointer that was set before the handler was installed.
        //
        // We use a separate lock-free snapshot vector of raw pointers updated
        // whenever regions_ changes, behind an atomic generation counter.
        LinuxRegionEntry* entry = find_entry_lockfree(addr);
        if (!entry) {
            char errmsg[256];
            int len = snprintf(errmsg, sizeof(errmsg), "handle_fault: entry not found for addr %p. Registered regions: ", addr);
            write(STDERR_FILENO, errmsg, len);
            for (size_t i = 0; i < snapshot_count_.load(std::memory_order_acquire); ++i) {
                auto* e = snapshot_.load(std::memory_order_acquire)[i];
                len = snprintf(errmsg, sizeof(errmsg), "[%p - %p] ", e->base, (char*)e->base + e->size);
                write(STDERR_FILENO, errmsg, len);
            }
            write(STDERR_FILENO, "\n", 1);
            std::abort();
            return false;
        }

        // Write the fault address to the worker pipe.  write(2) is async-
        // signal-safe.  The payload is a pointer (8 bytes on 64-bit).
        // We also write the entry pointer so the worker knows which semaphore
        // to post after calling the callback.
        sem_t local_sem;
        sem_init(&local_sem, 0, 0);

        FaultMsg msg{addr, entry, &local_sem};
        // Use a retry loop in case of EINTR.
        ssize_t written = 0;
        while (written < static_cast<ssize_t>(sizeof(msg))) {
            ssize_t n = write(pipe_write_fd_, reinterpret_cast<char*>(&msg) + written,
                              sizeof(msg) - written);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (n <= 0) {
                // Pipe is full or broken — can't handle. Let the fault propagate.
                sem_destroy(&local_sem);
                char errmsg[] = "handle_fault: pipe write failed\n";
                write(STDERR_FILENO, errmsg, sizeof(errmsg) - 1);
                std::abort();
                return false;
            }
            written += n;
        }

        // Block the faulting thread until the worker has called the callback
        // and made the page accessible.  sem_wait is async-signal-safe.
        while (sem_wait(&local_sem) == -1 && errno == EINTR) {}
        sem_destroy(&local_sem);

        return true;
    }

private:
    struct FaultMsg {
        void*              fault_addr;
        LinuxRegionEntry*  entry;
        sem_t*             thread_sem;
    };

    GlobalFaultHandler() {
        // Create a pipe for signal-to-worker communication.
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) != 0) {
            throw std::system_error(errno, std::generic_category(), "pipe2 failed");
        }
        // Make the write end non-blocking so the signal handler never stalls.
        int flags = fcntl(fds[1], F_GETFL);
        fcntl(fds[1], F_SETFL, flags | O_NONBLOCK);

        pipe_read_fd_  = fds[0];
        pipe_write_fd_ = fds[1];

        // Start the worker thread.
        running_.store(true, std::memory_order_release);
        worker_ = std::thread([this]() { worker_loop(); });

        pthread_atfork(nullptr, nullptr, []() {
            GlobalFaultHandler::instance().reinit_after_fork();
        });
    }

    void reinit_after_fork() {
        close(pipe_read_fd_);
        close(pipe_write_fd_);
        
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) == 0) {
            int flags = fcntl(fds[1], F_GETFL);
            fcntl(fds[1], F_SETFL, flags | O_NONBLOCK);
            pipe_read_fd_  = fds[0];
            pipe_write_fd_ = fds[1];
        }

        running_.store(true, std::memory_order_release);
        worker_ = std::thread([this]() { worker_loop(); });
    }

    ~GlobalFaultHandler() {
        running_.store(false, std::memory_order_release);
        close(pipe_write_fd_);
        close(pipe_read_fd_);
        if (worker_.joinable()) worker_.join();
    }

    void install_handler_once() {
        bool expected = false;
        if (!handler_installed_.compare_exchange_strong(expected, true)) return;

        struct sigaction sa{};
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sa.sa_sigaction = &GlobalFaultHandler::sig_handler;
        sigaction(SIGSEGV, &sa, &old_segv_action_);
        sigaction(SIGBUS,  &sa, &old_bus_action_);
    }

    // Async-signal-safe lock-free lookup.
    // We maintain a snapshot vector of raw pointers protected by an atomic
    // flag.  The signal handler reads the snapshot without locking.
    LinuxRegionEntry* find_entry_lockfree(void* addr) {
        // Spin until no writer is updating the snapshot.
        while (snapshot_writing_.load(std::memory_order_acquire)) {}

        char* a = static_cast<char*>(addr);
        LinuxRegionEntry** snap = snapshot_.load(std::memory_order_acquire);
        std::size_t count       = snapshot_count_.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < count; ++i) {
            LinuxRegionEntry* e = snap[i];
            char* base = static_cast<char*>(e->base);
            if (a >= base && a < base + e->size) return e;
        }
        return nullptr;
    }

    // Rebuild the lock-free snapshot.  Called from normal thread context
    // whenever regions_ changes.
    void rebuild_snapshot() {
        snapshot_writing_.store(true, std::memory_order_release);

        delete[] snapshot_buf_;
        snapshot_buf_   = new LinuxRegionEntry*[regions_.size() + 1];
        std::size_t i   = 0;
        for (auto& r : regions_) snapshot_buf_[i++] = r.get();

        snapshot_.store(snapshot_buf_, std::memory_order_release);
        snapshot_count_.store(regions_.size(), std::memory_order_release);
        snapshot_writing_.store(false, std::memory_order_release);
    }

    // Worker thread: reads fault messages from the pipe and processes them.
    void worker_loop() {
        FaultMsg msg;
        while (running_.load(std::memory_order_acquire)) {
            ssize_t nread = 0;
            while (nread < static_cast<ssize_t>(sizeof(msg))) {
                ssize_t n = read(pipe_read_fd_,
                                 reinterpret_cast<char*>(&msg) + nread,
                                 sizeof(msg) - nread);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    goto done;  // Pipe closed — shutting down.
                }
                if (n == 0) goto done;
                nread += n;
            }

            // Call the registered callback in a normal thread context.
            // All mutexes, condition variables, and network I/O are safe here.
            if (msg.entry && msg.entry->callback) {
                msg.entry->callback(msg.fault_addr);
            }

            // Wake the faulting thread.
            sem_post(msg.thread_sem);
        }
    done:;
    }

    static void sig_handler(int sig, siginfo_t* si, void* ctx) {
        if ((sig == SIGSEGV || sig == SIGBUS) &&
            GlobalFaultHandler::instance().handle_fault_from_signal(si->si_addr)) {
            return;  // Fault handled — resume execution.
        }
        // Not our fault — chain to the previous handler.
        struct sigaction& old = (sig == SIGSEGV)
            ? GlobalFaultHandler::instance().old_segv_action_
            : GlobalFaultHandler::instance().old_bus_action_;
        if (old.sa_flags & SA_SIGINFO) {
            if (old.sa_sigaction) old.sa_sigaction(sig, si, ctx);
        } else {
            if (old.sa_handler == SIG_DFL) { signal(sig, SIG_DFL); raise(sig); }
            else if (old.sa_handler != SIG_IGN) { old.sa_handler(sig); }
        }
    }

    int pipe_read_fd_{-1};
    int pipe_write_fd_{-1};

    std::mutex    mutex_;
    std::vector<std::unique_ptr<LinuxRegionEntry>> regions_;

    // Lock-free snapshot for signal-handler lookup.
    std::atomic<bool>                  snapshot_writing_{false};
    std::atomic<LinuxRegionEntry**>    snapshot_{nullptr};
    std::atomic<std::size_t>           snapshot_count_{0};
    LinuxRegionEntry**                 snapshot_buf_{nullptr};

    std::atomic<bool> handler_installed_{false};
    std::atomic<bool> running_{false};
    std::thread       worker_;

    struct sigaction old_segv_action_{};
    struct sigaction old_bus_action_{};
};

// ---------------------------------------------------------------------------
// FaultHandlerLinux — per-object wrapper around the global singleton.
// ---------------------------------------------------------------------------
class FaultHandlerLinux : public FaultHandler {
public:
    void register_region(void* base_address, std::size_t size, PageFaultCallback callback) override {
        GlobalFaultHandler::instance().register_region(base_address, size, std::move(callback));
        // Rebuild the lock-free snapshot so the signal handler can find the new entry.
        // rebuild_snapshot is called inside register_region after pushing to regions_.
    }

    void unregister_region(void* base_address) override {
        GlobalFaultHandler::instance().unregister_region(base_address);
    }
};

std::unique_ptr<FaultHandler> FaultHandler::create() {
    return std::make_unique<FaultHandlerLinux>();
}

// ============================================================================
// macOS / other POSIX — sigaction(SIGSEGV)
// ============================================================================
#else

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
        return cb ? cb(address) : false;
    }

private:
    GlobalFaultRegistry() = default;

    void setup_handler_if_needed() {
        if (handler_installed_) return;
        struct sigaction sa;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sa.sa_sigaction = &GlobalFaultRegistry::sig_handler;
        sigaction(SIGSEGV, &sa, &old_segv_action_);
        sigaction(SIGBUS,  &sa, &old_bus_action_);
        handler_installed_ = true;
    }

    static void sig_handler(int sig, siginfo_t* si, void* context) {
        if (sig == SIGSEGV || sig == SIGBUS) {
            if (GlobalFaultRegistry::instance().handle_fault(si->si_addr)) return;
        }
        struct sigaction& old = (sig == SIGSEGV)
            ? GlobalFaultRegistry::instance().old_segv_action_
            : GlobalFaultRegistry::instance().old_bus_action_;
        if (old.sa_flags & SA_SIGINFO) {
            if (old.sa_sigaction) old.sa_sigaction(sig, si, context);
        } else {
            if (old.sa_handler == SIG_DFL) { signal(sig, SIG_DFL); raise(sig); }
            else if (old.sa_handler != SIG_IGN) { old.sa_handler(sig); }
        }
    }

    std::mutex mutex_;
    std::vector<RegionEntry> entries_;
    bool handler_installed_{false};
    struct sigaction old_segv_action_;
    struct sigaction old_bus_action_;
};

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

#endif  // platform

} // namespace geryon
