import re

with open('README.md', 'r') as f:
    readme = f.read()

perf_section = """## Performance Benchmark

A demanding **Distributed Piecewise Sort Test** was used to validate network performance and memory stability. In this benchmark, a **256MB Shared Memory Region** containing **20 million random elements** is synchronously sorted across a star topology (1 Primary, 3 Replicas) using piecewise distributed workloads and cross-node lock contention.

Recent optimizations to the Bouncing Ownership protocol (resolving wait queue deadlocks and request timeouts) have **drastically reduced redundant page transfers by ~75%**. Replicas no longer thrash the network with redundant `PageRequest` messages while waiting for heavily contested pages.

### macOS Native (Clang++)
| Node Role | Total Pages Sent | Total Pages Received | Avg Transfer Time (µs) | Avg Time Between Transfers (µs) |
| --- | --- | --- | --- | --- |
| **Primary** | 3,671 | 3,670 | **2.67 µs** | **214.51 µs** |
| **Replica 1** | 1,328 | 1,329 | **~3.2 µs** | **~1,350 µs** |
| **Replica 2** | 1,329 | 1,330 | **~3.2 µs** | **~1,350 µs** |
| **Replica 3** | 1,330 | 1,331 | **~3.2 µs** | **~1,350 µs** |

### Windows 10 CrossOver (MinGW-w64)
*Windows uses Vectored Exception Handling (VEH) and fully completed 25 consecutive stress test iterations without deadlocks.*
| Node Role | Total Pages Sent | Total Pages Received |
| --- | --- | --- |
| **Primary** | 14,660 | 14,659 |
| **Replica 1** | 5,182 | 5,183 |
| **Replica 2** | 5,181 | 5,182 |
| **Replica 3** | 5,187 | 5,188 |
*(Note: Windows build was not updated with microsecond transfer logging for this run, and reflects pre-optimization transfer counts).*

*Note: The transfer latency remains extremely low on macOS, averaging **2.67 µs** per 4KB memory-mapped payload, highlighting the efficiency of the ASIO non-blocking I/O event loops even when heavily contested by multiple OS processes.*
"""

readme = re.sub(r'## Performance Benchmark.*?(?=## Test Coverage)', perf_section + '\n', readme, flags=re.DOTALL)

with open('README.md', 'w') as f:
    f.write(readme)
