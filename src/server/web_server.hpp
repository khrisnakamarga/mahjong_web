#pragma once

#include "server/room_server.hpp"

#include <crow.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace mahjong::server {

// HTTP + WebSocket front-end for RoomManager. The server is stateless beyond
// the in-memory RoomManager and a websocket-connection registry; restarting
// the process drops in-progress rooms.
class WebServer {
 public:
  WebServer(std::string staticDir, std::string publicBaseUrl,
            std::chrono::milliseconds idleActMs = std::chrono::milliseconds(10000),
            std::chrono::milliseconds idleTakeoverMs = std::chrono::milliseconds(90000));
  ~WebServer();

  // Blocks until the server stops. Returns the process exit code.
  int run(int port);

 private:
  struct ConnectionState {
    std::string roomCode;
    std::optional<int> seatIndex;        // empty for spectators
    std::string sessionToken;            // empty for spectators
  };

  std::string staticDir_;
  std::string publicBaseUrl_;

  // Idle-resilience knobs (configured at construction from env vars). The
  // background AI worker uses these to drive RoomManager::tickIdleHumans so
  // a disconnected or unresponsive seat does not soft-lock the game.
  std::chrono::milliseconds idleActMs_;
  std::chrono::milliseconds idleTakeoverMs_;

  RoomManager manager_;
  std::mutex stateMutex_;                // guards manager_ and connection registries

  crow::SimpleApp app_;

  // Connection registry. Pointers are stable for the lifetime of a connection.
  std::unordered_map<crow::websocket::connection*, ConnectionState> connections_;
  std::unordered_map<std::string, std::unordered_set<crow::websocket::connection*>> roomConnections_;

  // Background AI worker that drives AI moves on a per-room timer when
  // aiDelayMs > 0. Runs every ~25 ms; under stateMutex_ it scans rooms, ticks
  // any whose nextAiAt is due, and broadcasts a snapshot to the room.
  std::thread aiWorker_;
  std::atomic<bool> aiWorkerStop_{false};
  std::condition_variable aiWorkerCv_;
  void runAiWorker();

  // Background cleanup worker. Wakes every `cleanupIntervalSec_` seconds and
  // evicts rooms idle past their TTL or over the size cap. Connections to
  // evicted rooms are closed with a "room_evicted" error.
  std::thread cleanupWorker_;
  std::atomic<bool> cleanupWorkerStop_{false};
  std::condition_variable cleanupWorkerCv_;
  std::chrono::seconds ttlFinished_{1800};   // 30 min default
  std::chrono::seconds ttlActive_{21600};    // 6 h default
  std::chrono::seconds cleanupIntervalSec_{60};
  std::size_t maxRooms_{5000};
  void runCleanupWorker();

  // HTTP setup
  void registerRoutes();
  crow::response serveStaticFile(const std::string& relativePath) const;

  // WS handlers
  void onOpen(crow::websocket::connection& conn);
  void onClose(crow::websocket::connection& conn, const std::string& reason);
  void onMessage(crow::websocket::connection& conn, const std::string& data, bool isBinary);

  // Helpers
  void broadcastRoom(const std::string& roomCode);
  void sendSnapshotLocked(crow::websocket::connection& conn, const ConnectionState& state) const;
  void sendError(crow::websocket::connection& conn, const std::string& code, const std::string& message) const;
};

} // namespace mahjong::server
