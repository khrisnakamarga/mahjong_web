# Multi-stage build: GCC builds the binary, distroless-ish runtime serves it.
# Build:   docker build -t mahjong-web .
# Run:     docker run --rm -p 8080:8080 mahjong-web
# The image bundles the web/ static folder under /app/web.

# ---------- builder ----------
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src

# Build only the web server target. GUI is Windows-only; CLI demos and the unit
# test target aren't needed in the runtime image. FetchContent will download
# Crow, asio, and nlohmann/json during the configure step (requires network
# during build).
RUN cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DMAHJONG_BUILD_GUI=OFF \
      -DMAHJONG_BUILD_TESTS=OFF \
  && cmake --build build --target mahjong_web_server -j

# ---------- runtime ----------
FROM debian:bookworm-slim AS runtime

# A small runtime base; libstdc++ from the OS is sufficient.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --shell /usr/sbin/nologin app

WORKDIR /app
COPY --from=builder /src/build/mahjong_web_server /app/mahjong_web_server
COPY web /app/web

ENV PORT=8080
ENV MAHJONG_WEB_DIR=/app/web

EXPOSE 8080
USER app

# Simple TCP health check via the /api/health route would be ideal, but curl is
# not in the slim image. Container Apps probes will hit /api/health over HTTP.
ENTRYPOINT ["/app/mahjong_web_server"]
