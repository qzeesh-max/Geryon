FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    wget \
    curl \
    pkg-config \
    libboost-system-dev \
    libboost-thread-dev \
    libboost-program-options-dev \
    libboost-test-dev

WORKDIR /app
COPY . /app

# Build Geryon natively on Linux
RUN mkdir -p build-linux && cd build-linux && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc)

CMD sh -c "cd build-linux && ctest --output-on-failure"
