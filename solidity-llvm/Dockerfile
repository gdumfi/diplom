# solidity-llvm: контейнер для сборки и запуска компилятора.
#
# Сборка образа:
#   docker build -t solidity-llvm .
#
# Запуск (печать IR в stdout):
#   docker run --rm -v "$PWD/tests/fixtures:/work" solidity-llvm \
#       --input /work/counter.sol --emit-ir
#
# Демо без монтирования (внутри образа уже лежат фикстуры в /opt/fixtures):
#   docker run --rm solidity-llvm --input /opt/fixtures/counter.sol --emit-ir

# ---------- stage 1: сборка ----------
FROM ubuntu:24.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        git \
        ca-certificates \
        llvm-18-dev \
        libllvm18 \
        zlib1g-dev \
        libzstd-dev \
    && rm -rf /var/lib/apt/lists/*

ENV PATH=/usr/lib/llvm-18/bin:$PATH

WORKDIR /src
COPY . /src

RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build \
 && ctest --test-dir build --output-on-failure

# ---------- stage 2: рантайм ----------
FROM ubuntu:24.04 AS runtime

ARG DEBIAN_FRONTEND=noninteractive

# llvm-18 даёт нам llvm-as / llvm-dis / opt / llc и тянет за собой libllvm18.
RUN apt-get update && apt-get install -y --no-install-recommends \
        llvm-18 \
    && rm -rf /var/lib/apt/lists/*

ENV PATH=/usr/lib/llvm-18/bin:$PATH

COPY --from=builder /src/build/src/solidity-llvm /usr/local/bin/solidity-llvm
COPY --from=builder /src/tests/fixtures /opt/fixtures

WORKDIR /work
ENTRYPOINT ["solidity-llvm"]
CMD ["--help"]
