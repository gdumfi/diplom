# syntax=docker/dockerfile:1
# Полный образ витрины SCSA: анализатор (C++/LLVM) + фронт (Vite) + Go-бэкенд.
# Build context = КОРЕНЬ репо (~/diplom). Запуск — через deploy/docker-compose.yml.
#
# ВАЖНО: бинарь в рантайме зовёт `docker run ethereum/solc:…` (через stdin, без -v),
# поэтому контейнеру бэкенда монтируется docker-сокет ХОСТА (docker-out-of-docker).
# Демон берём с хоста — внутри образа только КЛИЕНТ docker.

# ---------- 1) анализатор (C++/LLVM-18) ----------
FROM ubuntu:24.04 AS sca-build
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git ca-certificates \
      llvm-18-dev libllvm18 zlib1g-dev libzstd-dev \
    && rm -rf /var/lib/apt/lists/*
ENV PATH=/usr/lib/llvm-18/bin:$PATH
COPY solidity-llvm/ /src/
WORKDIR /src
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build --target solidity-llvm

# ---------- 2) фронт (Vite) ----------
FROM node:22-slim AS web-build
WORKDIR /app
COPY web/package.json web/package-lock.json ./
RUN npm ci
COPY web/ ./
RUN npm run build            # tsc + vite build → /app/dist

# ---------- 3) Go-бэкенд (статический бинарь) ----------
FROM golang:1.23 AS go-build
WORKDIR /app
COPY web/server/ ./
RUN CGO_ENABLED=0 go build -trimpath -o /out/scsa-server .

# ---------- 4) рантайм ----------
FROM ubuntu:24.04 AS runtime
ARG DEBIAN_FRONTEND=noninteractive
ARG DOCKER_CLI_VERSION=27.3.1
RUN apt-get update && apt-get install -y --no-install-recommends \
      llvm-18 curl ca-certificates python3 \
    && rm -rf /var/lib/apt/lists/*
# python3 нужен пути `--address`: EtherscanFetch парсит ответ Etherscan через python3
# только клиент docker (демон — с хоста через смонтированный сокет)
RUN curl -fsSL "https://download.docker.com/linux/static/stable/x86_64/docker-${DOCKER_CLI_VERSION}.tgz" \
      | tar xz -C /usr/local/bin --strip-components=1 docker/docker
ENV PATH=/usr/lib/llvm-18/bin:$PATH
COPY --from=sca-build /src/build/src/solidity-llvm /usr/local/bin/solidity-llvm
COPY --from=web-build /app/dist /app/dist
COPY --from=go-build  /out/scsa-server /usr/local/bin/scsa-server
ENV SCA_BIN=/usr/local/bin/solidity-llvm \
    WEB_DIST=/app/dist \
    PORT=8080
EXPOSE 8080
CMD ["scsa-server"]
