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

/**
 * @file test_memnon_segmented.cpp
 * @brief Integration test: Memnon segmented managed memory + Geryon transaction log.
 *
 * Architecture
 * ────────────
 * Two processes (primary and replica) share two memory spaces:
 *
 *   1. Geryon MemoryRegion  — a TCP-synchronized region holding an
 *      append-only TransactionLog.  Each entry carries the Memnon named
 *      object key, the FNV-1a checksum of the payload, and the word count,
 *      so the replica can locate and fully validate the data it receives.
 *
 *   2. Memnon segmented_managed_memory — a POSIX-SHM-backed allocator that
 *      starts at 1 MiB and grows across multiple sub-segments as the primary
 *      commits large payloads.  Both processes open the same SHM root name;
 *      the replica can call find<DataRecord>(name) to obtain a direct pointer
 *      into the shared segment and verify every byte.
 *
 * Workflow
 * ────────
 *   Primary:
 *     for each transaction i:
 *       1. construct<DataRecord>(name) in Memnon — fills the initial segment,
 *          forcing Memnon to grow a new sub-segment on subsequent iterations.
 *       2. Fill every element with a deterministic pattern keyed by tx_id.
 *       3. Compute FNV-1a checksum of all data words.
 *       4. Append a LogEntry {name, checksum, word_count, tx_id} to the
 *          Geryon log and advance the atomic tail.
 *
 *   Replica:
 *     for each transaction i:
 *       1. Spin on the Geryon log tail (page faults pull data from primary).
 *       2. Call memnon.find<DataRecord>(entry.name) — reads actual bytes from
 *          the POSIX SHM segment; no data is re-derived locally.
 *       3. Verify tx_id, word_count, every individual data element, and the
 *          FNV-1a checksum against what the primary wrote into the Geryon log.
 *       4. On any mismatch, record the failure in the log and set
 *          replica_result=FAIL before signalling replica_done.
 *
 *   Primary (after replica_done):
 *     Asserts replica_result == RESULT_OK so GTest catches any replica failures.
 */

#include <gtest/gtest.h>
#include "geryon/memory_region.hpp"
#include "geryon/fault_handler.hpp"
#include "geryon/network_node.hpp"
#include "segmented_interprocess.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace geryon;
namespace si = segmented_interprocess;

// ============================================================================
// Constants
// ============================================================================

static constexpr uint16_t kPort            = 15780;
static constexpr int      kNumTransactions = 6;
// Each DataRecord is ~480 KB; with 6 records we exceed the 1 MiB initial
// Memnon segment, forcing it to grow across multiple sub-segments.
static constexpr uint32_t kPayloadWords    = 120'000;
// The SHM name is passed dynamically to avoid conflicts

// ============================================================================
// DataRecord — lives inside the Memnon segmented shared memory
//
// Fixed-size array so that Memnon's construct<DataRecord>(name)() works and
// the replica can call find<DataRecord>(name) to get a typed pointer directly
// into the shared segment.
// ============================================================================

struct DataRecord {
    uint32_t tx_id;
    uint32_t word_count;
    uint32_t data[kPayloadWords];
};

// ============================================================================
// Shared data structures (live inside the Geryon MemoryRegion)
// ============================================================================

// Replica result codes written back into the log so the primary can assert.
static constexpr uint32_t RESULT_PENDING = 0;
static constexpr uint32_t RESULT_OK      = 1;
static constexpr uint32_t RESULT_FAIL    = 2;

struct LogEntry {
    char     name[64];       ///< Memnon construct<DataRecord> key
    uint32_t checksum;       ///< FNV-1a checksum of all data[] words
    uint32_t payload_words;  ///< Always kPayloadWords
    uint32_t tx_id;          ///< Sequence number (0-based)

    // ── Replica validation results for this entry ──────────────────────────
    // Written by the replica after it reads the data from Memnon.
    uint32_t replica_tx_id_seen;      ///< DataRecord::tx_id read from Memnon
    uint32_t replica_checksum_actual; ///< Checksum the replica computed
    uint32_t replica_first_bad_word;  ///< Index of first mismatched word (or ~0u)
    uint32_t replica_bad_expected;    ///< Expected value at that index
    uint32_t replica_bad_actual;      ///< Actual value at that index
};

struct TransactionLog {
    // Primary sets this to 1 once start_primary() has returned and the
    // Memnon SHM has been created.
    alignas(64) std::atomic<uint32_t> primary_ready{0};
    char _pad0[60];

    // Incremented by primary after each named-object commit.
    alignas(64) std::atomic<uint32_t> tail{0};
    char _pad1[60];

    // Replica writes RESULT_OK / RESULT_FAIL, then sets replica_done=1.
    alignas(64) std::atomic<uint32_t> replica_result{RESULT_PENDING};
    char _pad2[60];

    alignas(64) std::atomic<uint32_t> replica_done{0};
    char _pad3[60];

    LogEntry entries[kNumTransactions];
};

// ============================================================================
// Deterministic pattern & checksum helpers
// ============================================================================

/// Fill data[] with a deterministic per-tx pattern using additive mixing
/// (avoids XOR cancellation artifacts that produce all-zero checksums).
static void fill_payload(uint32_t* data, uint32_t word_count, uint32_t tx_id) {
    for (uint32_t i = 0; i < word_count; ++i) {
        data[i] = (tx_id * 0x9e3779b9u) + i * 0x6c62272eu + 0xdeadcafeu;
    }
}

/// Expected value at word index i for a given tx_id (same formula as above).
static inline uint32_t expected_word(uint32_t tx_id, uint32_t i) {
    return (tx_id * 0x9e3779b9u) + i * 0x6c62272eu + 0xdeadcafeu;
}

/// FNV-1a-inspired checksum over all data words; guaranteed non-zero.
static uint32_t compute_checksum(const uint32_t* data, uint32_t count) {
    uint32_t cs = 0x811c9dc5u;
    for (uint32_t i = 0; i < count; ++i) {
        cs ^= data[i];
        cs *= 0x01000193u;
    }
    return cs ? cs : 0xdeadbeefu;
}

// ============================================================================
// Helper: round up to OS page size
// ============================================================================

static std::size_t page_align(std::size_t n) {
    std::size_t ps = MemoryRegion::system_page_size();
    return (n + ps - 1) / ps * ps;
}

// ============================================================================
// Primary process logic
// ============================================================================

static void run_primary() {
    // ── Geryon log region ────────────────────────────────────────────────────
    const std::size_t log_size = page_align(sizeof(TransactionLog));
    MemoryRegion log_region(log_size);
    log_region.set_protection(log_region.base_address(), log_size,
                              PageProtection::ReadWrite);

    auto* log = new (log_region.base_address()) TransactionLog();

    NetworkNode primary_node(&log_region, /*is_primary=*/true);
    auto primary_fault = FaultHandler::create();
    primary_fault->register_region_with_io(
        log_region.base_address(), log_region.io_address(), log_size,
        [&](void* addr) -> bool { return primary_node.request_page(addr); });

    primary_node.start_primary(kPort);
    std::cout << "[Primary] Listening on port " << kPort << "\n";

    // ── Memnon segmented SHM (creator) ──────────────────────────────────────
    // Start at kMinSegmentSize (1 MiB) so we force growth after the first
    // DataRecord allocation (~480 KB * 2 > 1 MiB).
    // Generate a random dynamic SHM name to avoid lingering file conflicts
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::string shm_name = "geryon_memnon_test_" + std::to_string(now) + "_" + std::to_string(std::rand());

    si::segmented_managed_memory memnon(shm_name.c_str(), si::create_only,
                                        si::kMinSegmentSize);
    std::cout << "[Primary] Memnon created — segments: " << memnon.segment_count()
              << "  size: " << memnon.get_size() << " B\n";

    // Signal to replica that primary socket AND Memnon SHM are ready.
    log->primary_ready.store(1, std::memory_order_release);

    // ── Spawn replica subprocess ─────────────────────────────────────────────
    extern std::string g_exec_path;
    std::string cmd = "\"" + g_exec_path + "\""
        + " --gtest_filter=MemnonSegmentedTest.TransactionalGrowthAcrossNodes"
        + " --run_as_memnon_replica --shm_name=" + shm_name;

#ifdef _WIN32
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    std::string args_cmd = cmd;
    if (!CreateProcessA(NULL, &args_cmd[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        throw std::runtime_error("CreateProcessA failed");
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    cmd += " &";
    std::thread replica_launcher([cmd]() { std::system(cmd.c_str()); });
    replica_launcher.detach();
#endif

    // Brief pause to let the replica connect before we start committing.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // ── Commit transactions ──────────────────────────────────────────────────
    for (int i = 0; i < kNumTransactions; ++i) {
        char obj_name[64];
        std::snprintf(obj_name, sizeof(obj_name), "tx_record_%d", i);

        // construct<DataRecord> allocates the record inside the Memnon SHM
        // and registers it under obj_name so the replica can find<> it.
        DataRecord* rec = memnon.construct<DataRecord>(obj_name);
        if (!rec) throw std::runtime_error("Memnon construct<DataRecord> failed");

        rec->tx_id      = static_cast<uint32_t>(i);
        rec->word_count = kPayloadWords;
        fill_payload(rec->data, kPayloadWords, rec->tx_id);
        const uint32_t cs = compute_checksum(rec->data, kPayloadWords);

        std::cout << "[Primary] tx " << i
                  << " | segments: " << memnon.segment_count()
                  << " | free: "     << memnon.get_free_memory() << " B"
                  << " | checksum: 0x" << std::hex << cs << std::dec << "\n";

        // Populate the Geryon log entry.
        LogEntry& entry = log->entries[i];
        std::strncpy(entry.name, obj_name, sizeof(entry.name) - 1);
        entry.name[sizeof(entry.name) - 1] = '\0';
        entry.checksum      = cs;
        entry.payload_words = kPayloadWords;
        entry.tx_id         = static_cast<uint32_t>(i);

        // Make the entry visible to the replica via the Geryon log.
        log->tail.fetch_add(1, std::memory_order_release);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // ── Wait for replica acknowledgement ─────────────────────────────────────
    while (log->replica_done.load(std::memory_order_acquire) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const uint32_t replica_result = log->replica_result.load(std::memory_order_acquire);
    std::cout << "[Primary] Replica done. Result: "
              << (replica_result == RESULT_OK ? "OK" : "FAIL")
              << "  Final Memnon segments: " << memnon.segment_count() << "\n";

    // Surface any replica failures through GTest on the primary side.
    if (replica_result != RESULT_OK) {
        for (int i = 0; i < kNumTransactions; ++i) {
            const LogEntry& e = log->entries[i];
            if (e.replica_checksum_actual != 0 && e.replica_checksum_actual != e.checksum) {
                ADD_FAILURE() << "[Replica] tx " << i
                              << " checksum mismatch: expected 0x" << std::hex << e.checksum
                              << " got 0x" << e.replica_checksum_actual << std::dec;
            }
            if (e.replica_tx_id_seen != e.tx_id) {
                ADD_FAILURE() << "[Replica] tx " << i
                              << " tx_id mismatch: expected " << e.tx_id
                              << " got " << e.replica_tx_id_seen;
            }
            if (e.replica_first_bad_word != ~0u) {
                ADD_FAILURE() << "[Replica] tx " << i
                              << " word[" << e.replica_first_bad_word << "] corrupt:"
                              << " expected 0x" << std::hex << e.replica_bad_expected
                              << " got 0x"      << e.replica_bad_actual << std::dec;
            }
        }
        ADD_FAILURE() << "Replica reported validation failures (see above).";
    }

    primary_fault->unregister_region(log_region.base_address());
    primary_node.stop();
}

// ============================================================================
// Replica process logic
// ============================================================================

static void run_replica(const std::string& shm_name) {
    // ── Geryon log region ────────────────────────────────────────────────────
    const std::size_t log_size = page_align(sizeof(TransactionLog));
    MemoryRegion log_region(log_size);
    log_region.set_protection(log_region.base_address(), log_size,
                              PageProtection::None);

    NetworkNode replica_node(&log_region, /*is_primary=*/false);
    auto replica_fault = FaultHandler::create();
    replica_fault->register_region_with_io(
        log_region.base_address(), log_region.io_address(), log_size,
        [&](void* addr) -> bool { return replica_node.request_page(addr); });

    replica_node.start_replica("127.0.0.1", kPort);

    auto* log = reinterpret_cast<TransactionLog*>(log_region.base_address());

    // Wait for primary to signal that both the primary socket and the Memnon
    // SHM have been created before we try to open them.
    auto start_wait = std::chrono::steady_clock::now();
    while (log->primary_ready.load(std::memory_order_acquire) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start_wait > std::chrono::seconds(10)) {
            std::cerr << "[Replica] Timed out waiting for primary_ready" << std::endl;
            std::exit(1);
        }
    }

    // ── Open Memnon SHM (opener side) ───────────────────────────────────────
    std::unique_ptr<si::segmented_managed_memory> memnon;
    for (int attempt = 0; attempt < 40; ++attempt) {
        try {
            memnon = std::make_unique<si::segmented_managed_memory>(
                shm_name.c_str(), si::open_only);
            break;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    if (!memnon) {
        std::cerr << "[Replica] FATAL: could not open Memnon SHM\n";
        std::exit(2);
    }
    std::cout << "[Replica] Memnon opened — segments: " << memnon->segment_count() << "\n";

    // ── Validate each transaction ────────────────────────────────────────────
    bool all_ok = true;

    for (int i = 0; i < kNumTransactions; ++i) {
        // Spin until the primary commits transaction i (Geryon page faults
        // synchronize the log page from the primary over TCP).
        while (log->tail.load(std::memory_order_acquire) <= static_cast<uint32_t>(i)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        LogEntry& entry = log->entries[i];

        // Force discovery of any new Memnon sub-segments that the primary may
        // have grown since we opened.  lazy_discover_growth reads the segment
        // table header in sub-segment 0 and maps any new sub-segments in-process.
        memnon->get_segment_manager().lazy_discover_growth(
            std::numeric_limits<std::size_t>::max());

        // ── Step 1: Locate the DataRecord in Memnon SHM ─────────────────────
        // The primary used construct<DataRecord>(entry.name), so we can
        // retrieve a direct pointer into the shared segment — no copy, no
        // re-derivation; we read the actual bytes the primary wrote.
        auto [rec_ptr, count] = memnon->find<DataRecord>(entry.name);

        // Record the tx_id we actually saw (for primary-side reporting).
        entry.replica_tx_id_seen = rec_ptr ? rec_ptr->tx_id : ~0u;
        entry.replica_first_bad_word = ~0u; // sentinel: no corruption found

        if (!rec_ptr) {
            std::cerr << "[Replica] tx " << i << " — DataRecord '" << entry.name
                      << "' NOT FOUND in Memnon SHM\n";
            all_ok = false;
            entry.replica_checksum_actual = 0;
            continue;
        }

        // ── Step 2: Verify metadata fields ──────────────────────────────────
        if (rec_ptr->tx_id != entry.tx_id) {
            std::cerr << "[Replica] tx " << i << " tx_id mismatch in DataRecord: "
                      << "expected " << entry.tx_id
                      << " got "     << rec_ptr->tx_id << "\n";
            all_ok = false;
        }

        if (rec_ptr->word_count != entry.payload_words) {
            std::cerr << "[Replica] tx " << i << " word_count mismatch: "
                      << "expected " << entry.payload_words
                      << " got "     << rec_ptr->word_count << "\n";
            all_ok = false;
        }

        // ── Step 3: Verify every data word individually ──────────────────────
        // Reading directly from Memnon's POSIX shared memory — these are the
        // exact bytes written by the primary.
        const uint32_t words_to_check = std::min(rec_ptr->word_count, kPayloadWords);
        for (uint32_t w = 0; w < words_to_check; ++w) {
            const uint32_t expected = expected_word(entry.tx_id, w);
            const uint32_t actual   = rec_ptr->data[w];
            if (actual != expected) {
                if (entry.replica_first_bad_word == ~0u) {
                    // Record first corruption for primary-side reporting.
                    entry.replica_first_bad_word = w;
                    entry.replica_bad_expected   = expected;
                    entry.replica_bad_actual      = actual;
                }
                std::cerr << "[Replica] tx " << i
                          << " CORRUPT at word[" << w << "]:"
                          << " expected 0x" << std::hex << expected
                          << " got 0x"      << actual   << std::dec << "\n";
                all_ok = false;
                break; // report first corruption only; checksum will also fail
            }
        }

        // ── Step 4: Verify the FNV-1a checksum over all data words ──────────
        // This catches any silent corruption not caught by word-level checks.
        const uint32_t actual_cs = compute_checksum(rec_ptr->data, words_to_check);
        entry.replica_checksum_actual = actual_cs;

        if (actual_cs != entry.checksum) {
            std::cerr << "[Replica] tx " << i << " checksum MISMATCH:"
                      << " expected 0x" << std::hex << entry.checksum
                      << " got 0x"      << actual_cs << std::dec << "\n";
            all_ok = false;
        } else {
            std::cout << "[Replica] tx " << i << " OK"
                      << "  name=" << entry.name
                      << "  checksum=0x" << std::hex << actual_cs << std::dec
                      << "  words_checked=" << words_to_check
                      << "  Memnon_segs=" << memnon->segment_count() << "\n";
        }
    }

    // ── Signal primary ───────────────────────────────────────────────────────
    log->replica_result.store(all_ok ? RESULT_OK : RESULT_FAIL,
                              std::memory_order_release);
    log->replica_done.store(1, std::memory_order_release);

    replica_fault->unregister_region(log_region.base_address());
    replica_node.stop();

    std::exit(all_ok ? 0 : 1);
}

// ============================================================================
// GTest wrapper
// ============================================================================

std::string g_exec_path; // shared with run_primary()

TEST(MemnonSegmentedTest, TransactionalGrowthAcrossNodes) {
    // (Subprocess correctly spawned natively without cmd.exe via CreateProcessA)

    const auto& args = testing::internal::GetArgvs();

    bool is_replica = false;
    for (const auto& arg : args) {
        if (arg == "--run_as_memnon_replica") {
            is_replica = true;
            break;
        }
    }

    if (is_replica) {
        std::string shm_name = "geryon_memnon_txlog_test";
        for (const auto& arg : args) {
            if (arg.find("--shm_name=") == 0) {
                shm_name = arg.substr(11);
            }
        }
        run_replica(shm_name);
        // run_replica() calls std::exit(); this line is unreachable.
    }

    // Store the executable path for run_primary() to spawn the replica
    // subprocess after the primary socket is bound.
    g_exec_path = args[0];

    // Blocks until replica_done is set; then asserts replica_result == OK.
    ASSERT_NO_THROW(run_primary());

    // Allow the replica process a moment to exit cleanly.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}
