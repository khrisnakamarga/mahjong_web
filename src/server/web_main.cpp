#include "server/web_server.hpp"

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

  mahjong::server::WebServer server(staticDir, publicBaseUrl);
  return server.run(port);
}
