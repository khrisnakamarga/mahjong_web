#include "server/web_server.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

int parseIntEnv(const char* name, int fallback) {
  const char* raw = std::getenv(name);
  if (!raw) return fallback;
  try { return std::stoi(raw); } catch (...) { return fallback; }
}

std::string parseStringEnv(const char* name, std::string fallback) {
  const char* raw = std::getenv(name);
  return raw ? std::string(raw) : std::move(fallback);
}

std::string resolveDefaultStaticDir() {
  // Try a few common locations relative to the executable so that local dev,
  // CMake builds, and Docker images all work without configuration.
  namespace fs = std::filesystem;
  const fs::path candidates[] = {
      "web",                        // CWD relative (Docker workdir = /app)
      "../web",                     // build/ directory next to source
      "../../web",                  // nested out-of-tree builds
      "/app/web",                   // explicit Docker default
  };
  for (const auto& candidate : candidates) {
    if (fs::exists(candidate / "index.html")) return fs::absolute(candidate).string();
  }
  return fs::absolute("web").string();
}

} // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  const int port = parseIntEnv("PORT", 8080);
  const std::string staticDir = parseStringEnv("MAHJONG_WEB_DIR", resolveDefaultStaticDir());
  const std::string publicBaseUrl = parseStringEnv("MAHJONG_PUBLIC_BASE_URL", "");

  // Resiliency knobs. When a human seat hasn't acted for idleActMs and is
  // blocking the round, the server auto-passes (or picks an AI action) on
  // their behalf. After idleTakeoverMs the seat fully reverts to AI control;
  // the human can reclaim by re-using their seat link.
  const int idleActMs = parseIntEnv("MAHJONG_IDLE_ACT_MS", 10000);
  const int idleTakeoverMs = parseIntEnv("MAHJONG_IDLE_TAKEOVER_MS", 90000);

  mahjong::server::WebServer server(staticDir, publicBaseUrl,
                                    std::chrono::milliseconds(std::max(0, idleActMs)),
                                    std::chrono::milliseconds(std::max(0, idleTakeoverMs)));
  return server.run(port);
}
