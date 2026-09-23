# Credits

Geryon relies on several fantastic open-source projects. We are grateful to the authors and contributors of the following software:

## Core Dependencies
* **[Boost](https://www.boost.org/)** - Distributed under the [Boost Software License, Version 1.0](https://www.boost.org/LICENSE_1_0.txt). Used extensively for cross-platform interprocess communication, synchronization, networking (`Boost.Asio`), and UUID generation.
* **[Memnon](https://github.com/zqazi/memnon)** - Distributed under the [GNU Affero General Public License v3.0](https://www.gnu.org/licenses/agpl-3.0.html). Used as the core high-performance IPC backend when running nodes concurrently on the same machine.

## Testing and Benchmarking
* **[GoogleTest](https://github.com/google/googletest)** - Distributed under the [BSD 3-Clause License](https://opensource.org/licenses/BSD-3-Clause). Used for unit and correctness testing.
* **[Google Benchmark](https://github.com/google/benchmark)** - Distributed under the [Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0). Used for microbenchmarking synchronization overhead and page transfer latencies.

## Build System
* **[CMake](https://cmake.org/)** - Distributed under the [OSI-approved BSD 3-Clause License](https://cmake.org/licensing/). Used as the build system generator.
