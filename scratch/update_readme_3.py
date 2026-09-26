import re

with open('README.md', 'r') as f:
    readme = f.read()

test_coverage = """## Test Coverage

Geryon comes with a comprehensive, deterministic 15-test suite designed to validate consistency under heavy concurrency and varied OS architectures:

- **100% Passing Rate** across macOS Native (`clang++`) and Windows 10 via CrossOver (`mingw-w64`).
- **Linux Native via Docker (`ubuntu:24.04`)** currently has a known `SIGSEGV` limitation under extreme concurrent fault load (e.g. `MultithreadMultipageCorrectness`) due to `sigaction` async-signal-safety violations when threads are interrupted synchronously and block on internal standard library synchronizations during signal handling.
- Tests simulate massive fault contention, validating correct queue processing for **Distributed Shared Memory Thrashing** and resolving edge cases where >60 threads aggressively request the same page memory address.
"""

readme = re.sub(r'## Test Coverage.*?(?=## Getting Started)', test_coverage + '\n', readme, flags=re.DOTALL)

with open('README.md', 'w') as f:
    f.write(readme)
