# syntax=docker/dockerfile:1.7

FROM ubuntu:24.04 AS core-builder

ENV DEBIAN_FRONTEND=noninteractive

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && apt-get install --no-install-recommends -y \
      ca-certificates \
      cmake \
      g++ \
      gcc \
      git \
      libgl1-mesa-dev \
      libpcap-dev \
      make \
      ninja-build \
      pkg-config \
      python3 \
      zlib1g-dev

WORKDIR /src
COPY . .

RUN cmake -S . -B /build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_OPENGL=OFF \
      -DENABLE_TESTS=OFF && \
    cmake --build /build --config Release && \
    mkdir -p /out && \
    find /build -name '*melondsds*libretro*.so' -exec cp {} /out/melondsds_libretro.so \; && \
    test -f /out/melondsds_libretro.so

FROM node:22-bookworm-slim AS room

ENV NODE_ENV=production \
    PORT=8787 \
    ROM_PATH=/roms/demo.nds \
    ROOM_CONFIG=/app/room-config.json \
    RUNNER_SOCKET=/run/rebit-room.sock

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && apt-get install --no-install-recommends -y \
      ca-certificates \
      libpcap0.8 \
      libstdc++6 && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=core-builder /out/melondsds_libretro.so /opt/melonds/melondsds_libretro.so
COPY container/server/package.json ./server/package.json
RUN cd server && npm install --omit=dev

COPY container/server ./server
COPY container/web ./web
COPY container/room-config.example.json ./room-config.json
COPY container/runner-contract.md ./runner-contract.md

VOLUME ["/roms", "/runtime"]
EXPOSE 8787/tcp
EXPOSE 8443/udp

CMD ["node", "server/server.mjs"]
