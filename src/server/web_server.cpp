#include "server/web_server.hpp"

#include "server/json_codec.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace mahjong::server {
namespace {

using json = nlohmann::json;

std::string readFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

std::string mimeFor(const std::string& filename) {
  const auto dot = filename.find_last_of('.');
  if (dot == std::string::npos) return "application/octet-stream";
  const auto ext = filename.substr(dot);
  if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
  if (ext == ".css") return "text/css; charset=utf-8";
  if (ext == ".js" || ext == ".mjs") return "application/javascript; charset=utf-8";
  if (ext == ".json") return "application/json; charset=utf-8";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".png") return "image/png";
  if (ext == ".ico") return "image/x-icon";
  if (ext == ".woff2") return "font/woff2";
  if (ext == ".webmanifest") return "application/manifest+json";
  return "application/octet-stream";
}

bool pathTraverses(const std::string& relative) {
  if (relative.find("..") != std::string::npos) return true;
  if (!relative.empty() && relative.front() == '/') return true;
  return false;
}

crow::response jsonResponse(int status, json body) {
  crow::response resp(status, body.dump());
  resp.add_header("Content-Type", "application/json; charset=utf-8");
  return resp;
}

crow::response jsonOk(json body) { return jsonResponse(200, std::move(body)); }

} // namespace

WebServer::WebServer(std::string staticDir, std::string publicBaseUrl,
                     std::chrono::milliseconds idleActMs,
                     std::chrono::milliseconds idleTakeoverMs)
    : staticDir_(std::move(staticDir)),
      publicBaseUrl_(std::move(publicBaseUrl)),
      idleActMs_(idleActMs),
      idleTakeoverMs_(idleTakeoverMs),
      manager_(publicBaseUrl_.empty() ? std::string("local://mahjong") : publicBaseUrl_) {
  // Env-driven cleanup tuning. All values are seconds, room count for maxRooms.
  auto readPositiveEnv = [](const char* name, long long fallback) -> long long {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return fallback;
    try {
      const long long parsed = std::stoll(raw);
      return parsed < 0 ? fallback : parsed;
    } catch (...) { return fallback; }
  };
  ttlFinished_      = std::chrono::seconds(readPositiveEnv("MAHJONG_ROOM_TTL_FINISHED_SECONDS", 1800));
  ttlActive_        = std::chrono::seconds(readPositiveEnv("MAHJONG_ROOM_TTL_ACTIVE_SECONDS", 21600));
  cleanupIntervalSec_ = std::chrono::seconds(std::max<long long>(1, readPositiveEnv("MAHJONG_CLEANUP_INTERVAL_SECONDS", 60)));
  maxRooms_         = static_cast<std::size_t>(readPositiveEnv("MAHJONG_MAX_ROOMS", 5000));
  std::cout << "[cleanup] ttlFinished=" << ttlFinished_.count() << "s"
            << " ttlActive=" << ttlActive_.count() << "s"
            << " interval=" << cleanupIntervalSec_.count() << "s"
            << " maxRooms=" << maxRooms_ << std::endl;
  std::cout << "[resiliency] idleActMs=" << idleActMs_.count()
            << " idleTakeoverMs=" << idleTakeoverMs_.count() << std::endl;

  registerRoutes();
  aiWorker_ = std::thread([this] { runAiWorker(); });
  cleanupWorker_ = std::thread([this] { runCleanupWorker(); });
}

WebServer::~WebServer() {
  aiWorkerStop_.store(true);
  aiWorkerCv_.notify_all();
  cleanupWorkerStop_.store(true);
  cleanupWorkerCv_.notify_all();
  if (aiWorker_.joinable()) aiWorker_.join();
  if (cleanupWorker_.joinable()) cleanupWorker_.join();
}

void WebServer::runCleanupWorker() {
  while (!cleanupWorkerStop_.load()) {
    {
      std::unique_lock<std::mutex> lock(stateMutex_);
      cleanupWorkerCv_.wait_for(lock, cleanupIntervalSec_, [this] { return cleanupWorkerStop_.load(); });
      if (cleanupWorkerStop_.load()) break;

      const auto now = std::chrono::steady_clock::now();
      const auto evicted = manager_.evictIdleRooms(now, ttlFinished_, ttlActive_, maxRooms_);
      if (evicted.empty()) continue;

      // For each evicted room, notify and close any still-attached connections.
      for (const auto& code : evicted) {
        const auto it = roomConnections_.find(code);
        if (it == roomConnections_.end()) continue;
        // Snapshot pointers so we can mutate the maps while iterating.
        std::vector<crow::websocket::connection*> conns(it->second.begin(), it->second.end());
        for (auto* conn : conns) {
          json payload{
              {"type", "error"},
              {"code", "room_evicted"},
              {"message", "Room expired due to inactivity"},
          };
          try { conn->send_text(payload.dump()); } catch (...) { /* swallow */ }
          try { conn->close("room_evicted"); } catch (...) { /* swallow */ }
          connections_.erase(conn);
        }
        roomConnections_.erase(code);
      }
      std::cout << "[cleanup] evicted " << evicted.size() << " idle room(s); "
                << manager_.roomCount() << " remaining" << std::endl;
    }
  }
}

void WebServer::runAiWorker() {
  using namespace std::chrono_literals;
  while (!aiWorkerStop_.load()) {
    std::unique_lock<std::mutex> lock(stateMutex_);
    // Find the earliest pending deadline across all rooms (AI move OR idle
    // recovery). We iterate ALL rooms, not just rooms with WS connections,
    // so that an unattended seat still gets auto-passed / converted to AI
    // even when the last human disconnects. Broadcasts are only sent to
    // rooms that actually have live connections.
    auto now = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point earliest = now + std::chrono::seconds(60);
    bool anyPending = false;
    const auto rooms = manager_.listRoomCodes();

    auto broadcastIfConnected = [&](const std::string& code) {
      const auto it = roomConnections_.find(code);
      if (it == roomConnections_.end()) return;
      for (auto* conn : it->second) {
        const auto cit = connections_.find(conn);
        if (cit == connections_.end()) continue;
        sendSnapshotLocked(*conn, cit->second);
      }
    };

    // ---- Tick due AI moves and idle recoveries ----
    for (const auto& code : rooms) {
      // 1) Normal AI cascade for AI-controlled seats.
      auto aiDue = manager_.nextAiDueAt(code);
      if (aiDue) {
        anyPending = true;
        if (*aiDue <= now) {
          if (manager_.tickAi(code)) broadcastIfConnected(code);
          auto next = manager_.nextAiDueAt(code);
          if (next && *next < earliest) earliest = *next;
        } else if (*aiDue < earliest) {
          earliest = *aiDue;
        }
      }

      // 2) Idle resilience: auto-pass or AI-takeover human seats that have
      //    been blocking the round too long. Always run a tick: the function
      //    is a cheap no-op when no human seat has pending actions, and a
      //    single tick is needed to *seed* the pendingActionSince markers
      //    that nextIdleDueAt relies on. After the tick, query for the next
      //    deadline so we can sleep efficiently.
      auto outcome = manager_.tickIdleHumans(code, idleActMs_, idleTakeoverMs_);
      if (outcome.changed) broadcastIfConnected(code);
      // Push session_invalidated to any WS connections still bound to a
      // seat that just got converted to AI so the displaced browser can
      // gracefully demote itself instead of issuing further stale-token
      // actions.
      if (!outcome.seatsConvertedToAi.empty()) {
        const auto cit = roomConnections_.find(code);
        if (cit != roomConnections_.end()) {
          for (auto* conn : cit->second) {
            const auto sit = connections_.find(conn);
            if (sit == connections_.end()) continue;
            if (!sit->second.seatIndex) continue;
            const int s = *sit->second.seatIndex;
            if (std::find(outcome.seatsConvertedToAi.begin(),
                          outcome.seatsConvertedToAi.end(), s) ==
                outcome.seatsConvertedToAi.end()) continue;
            sendError(*conn, "session_invalidated",
                      "You were idle too long; the AI has taken over your seat. Reconnect via your seat link to reclaim.");
            sit->second.seatIndex.reset();
            sit->second.sessionToken.clear();
          }
        }
      }
      auto idleDue = manager_.nextIdleDueAt(code, idleActMs_, idleTakeoverMs_);
      if (idleDue) {
        anyPending = true;
        if (*idleDue < earliest) earliest = *idleDue;
      }
    }
    if (aiWorkerStop_.load()) break;
    // Sleep until earliest deadline or 250ms, whichever is smaller. Wake on
    // notify (new action submitted or delay changed).
    auto waitFor = earliest - std::chrono::steady_clock::now();
    if (!anyPending) waitFor = std::chrono::milliseconds(250);
    if (waitFor < std::chrono::milliseconds(5)) waitFor = std::chrono::milliseconds(5);
    if (waitFor > std::chrono::milliseconds(250)) waitFor = std::chrono::milliseconds(250);
    aiWorkerCv_.wait_for(lock, waitFor);
  }
}

int WebServer::run(int port) {
  std::cout << "Mahjong web server listening on port " << port
            << " (all interfaces). Open http://localhost:" << port
            << "/ in your browser." << std::endl;
  std::cout << "Static dir: " << staticDir_ << std::endl;
  if (!publicBaseUrl_.empty()) std::cout << "Public base URL: " << publicBaseUrl_ << std::endl;
  app_.port(static_cast<std::uint16_t>(port)).multithreaded().run();
  return 0;
}

crow::response WebServer::serveStaticFile(const std::string& relativePath) const {
  if (pathTraverses(relativePath)) return crow::response(400, "bad path");
  std::filesystem::path full = std::filesystem::path(staticDir_) / relativePath;
  if (!std::filesystem::exists(full) || std::filesystem::is_directory(full)) {
    full = std::filesystem::path(staticDir_) / "index.html";
    if (!std::filesystem::exists(full)) return crow::response(404, "not found");
  }
  const auto body = readFile(full);
  crow::response resp(body);
  resp.add_header("Content-Type", mimeFor(full.filename().string()));
  resp.add_header("Cache-Control", "no-cache");
  return resp;
}

void WebServer::registerRoutes() {
  CROW_ROUTE(app_, "/api/health")([]() { return std::string("ok"); });

  CROW_ROUTE(app_, "/api/rooms").methods(crow::HTTPMethod::POST)(
      [this](const crow::request& req) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        std::string seed;
        if (!req.body.empty()) {
          try {
            const auto body = json::parse(req.body);
            if (body.contains("seed") && body.at("seed").is_string()) seed = body.at("seed").get<std::string>();
          } catch (...) { /* ignore malformed body, use defaults */ }
        }
        const auto result = manager_.createRoom(seed);
        json claimLinks = json::array();
        for (const auto& link : result.claimLinks) claimLinks.push_back(codec::toJson(link));
        return jsonOk({
            {"roomCode", result.room.roomCode},
            {"version", result.room.version},
            {"claimLinks", std::move(claimLinks)},
        });
      });

  CROW_ROUTE(app_, "/api/rooms/<string>")(
      [this](const std::string& code) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        const auto room = manager_.getRoom(code);
        if (!room) return jsonResponse(404, {{"error", "not_found"}});
        const auto snapshot = manager_.createSnapshot(*room);
        return jsonOk(codec::toJson(snapshot));
      });

  CROW_ROUTE(app_, "/api/rooms/<string>/seats/<int>").methods(crow::HTTPMethod::POST)(
      [this](const crow::request& req, const std::string& code, int seat) {
        json body;
        try { body = json::parse(req.body); } catch (...) { return jsonResponse(400, {{"error", "bad_json"}}); }
        const auto token = body.value("token", std::string{});
        const auto displayName = body.value("displayName", std::string{});
        const bool forceTakeover = body.value("forceTakeover", false);
        if (token.empty()) return jsonResponse(400, {{"error", "missing_token"}});
        try {
          std::lock_guard<std::mutex> lock(stateMutex_);
          const auto result = manager_.claimSeat(code, seat, token, displayName, forceTakeover);
          // Eject any existing WS connections that were bound to this seat
          // with the now-invalidated session token. They get a structured
          // error so the browser can demote itself instead of issuing further
          // stale-token actions.
          if (result.displacedSessionTokenHash) {
            const auto it = roomConnections_.find(result.room.roomCode);
            if (it != roomConnections_.end()) {
              for (auto* conn : it->second) {
                const auto cit = connections_.find(conn);
                if (cit == connections_.end()) continue;
                if (!cit->second.seatIndex || *cit->second.seatIndex != seat) continue;
                if (cit->second.sessionToken == result.sessionToken) continue;
                sendError(*conn, "session_invalidated",
                          "Another player took over your seat. You have been demoted to spectator.");
                cit->second.seatIndex.reset();
                cit->second.sessionToken.clear();
              }
            }
          }
          // Push a fresh snapshot to every WS connection already in the
          // room so they see the new player name immediately (rather than
          // waiting for the next AI move / setting toggle to trigger a
          // broadcast).
          broadcastRoom(result.room.roomCode);
          return jsonOk({
              {"roomCode", result.room.roomCode},
              {"version", result.room.version},
              {"seatIndex", seat},
              {"sessionToken", result.sessionToken},
          });
        } catch (const SeatInUseError& error) {
          return jsonResponse(409, {{"error", "seat_in_use"}, {"message", error.what()}});
        } catch (const std::exception& error) {
          return jsonResponse(403, {{"error", "claim_failed"}, {"message", error.what()}});
        }
      });

  CROW_WEBSOCKET_ROUTE(app_, "/ws")
      .onopen([this](crow::websocket::connection& conn) { onOpen(conn); })
      .onclose([this](crow::websocket::connection& conn, const std::string& reason) {
        onClose(conn, reason);
      })
      .onmessage([this](crow::websocket::connection& conn, const std::string& data, bool isBinary) {
        onMessage(conn, data, isBinary);
      });

  CROW_ROUTE(app_, "/")([this]() { return serveStaticFile("index.html"); });
  CROW_ROUTE(app_, "/<path>")([this](const std::string& path) { return serveStaticFile(path); });
}

void WebServer::onOpen(crow::websocket::connection& conn) {
  std::lock_guard<std::mutex> lock(stateMutex_);
  connections_[&conn] = ConnectionState{};
}

void WebServer::onClose(crow::websocket::connection& conn, const std::string& /*reason*/) {
  std::lock_guard<std::mutex> lock(stateMutex_);
  const auto it = connections_.find(&conn);
  if (it == connections_.end()) return;
  if (!it->second.roomCode.empty()) {
    auto& set = roomConnections_[it->second.roomCode];
    set.erase(&conn);
    if (set.empty()) roomConnections_.erase(it->second.roomCode);
    // Recompute the per-seat live-connection count for the seat this
    // connection was bound to. We can't simply decrement because there may
    // have been parallel browsers (desktop + mobile) for the same seat;
    // counting the remaining matching connections is authoritative.
    if (it->second.seatIndex) {
      const int seat = *it->second.seatIndex;
      int remaining = 0;
      const auto cit = roomConnections_.find(it->second.roomCode);
      if (cit != roomConnections_.end()) {
        for (auto* other : cit->second) {
          const auto oit = connections_.find(other);
          if (oit == connections_.end()) continue;
          if (oit->second.seatIndex && *oit->second.seatIndex == seat) remaining++;
        }
      }
      manager_.setSeatConnectionCount(it->second.roomCode, seat, remaining);
    }
  }
  connections_.erase(it);
}

void WebServer::onMessage(crow::websocket::connection& conn, const std::string& data, bool /*isBinary*/) {
  json message;
  try { message = json::parse(data); }
  catch (...) { sendError(conn, "bad_json", "Could not parse message as JSON"); return; }

  const auto type = message.value("type", std::string{});
  if (type == "ping") {
    conn.send_text(json{{"type", "pong"}}.dump());
    // Pings count as liveness for the seat. We deliberately do NOT reset the
    // pendingActionSince deadline here -- a frozen tab can still send pings;
    // the only thing that resolves a pending decision is a real action.
    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto it = connections_.find(&conn);
    if (it != connections_.end() && it->second.seatIndex && !it->second.roomCode.empty()) {
      manager_.noteSeatActivity(it->second.roomCode, *it->second.seatIndex);
    }
    return;
  }

  if (type == "hello") {
    const auto roomCode = normalizeRoomCode(message.value("roomCode", std::string{}));
    if (roomCode.empty()) { sendError(conn, "missing_room", "hello requires a roomCode"); return; }
    const auto sessionToken = message.value("sessionToken", std::string{});

    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto room = manager_.getRoom(roomCode);
    if (!room) { sendError(conn, "not_found", "Room does not exist"); return; }

    std::optional<int> seatIndex;
    if (!sessionToken.empty()) {
      // Probe each seat for a session token match by attempting a dummy
      // operation: we can't directly read the hash, so instead try a no-op
      // "Pass" if available, falling back to scanning seats and accepting
      // the first one whose claim succeeds. Simpler: trust the client to
      // tell us their seat index.
      if (message.contains("seatIndex") && message.at("seatIndex").is_number_integer()) {
        seatIndex = message.at("seatIndex").get<int>();
      }
    }

    auto& state = connections_[&conn];
    state.roomCode = roomCode;
    state.seatIndex = seatIndex;
    state.sessionToken = sessionToken;
    roomConnections_[roomCode].insert(&conn);
    manager_.touchRoom(roomCode);
    // Bump per-seat liveness so the idle worker resets its deadline and so
    // claimSeat treats the seat as actively held against silent overrides.
    if (seatIndex) {
      int liveCount = 0;
      for (auto* other : roomConnections_[roomCode]) {
        const auto oit = connections_.find(other);
        if (oit == connections_.end()) continue;
        if (oit->second.seatIndex && *oit->second.seatIndex == *seatIndex) liveCount++;
      }
      manager_.setSeatConnectionCount(roomCode, *seatIndex, liveCount);
      manager_.noteSeatActivity(roomCode, *seatIndex);
    }

    json welcome{
        {"type", "welcome"},
        {"roomCode", roomCode},
    };
    if (seatIndex) welcome["seatIndex"] = *seatIndex;
    conn.send_text(welcome.dump());
    sendSnapshotLocked(conn, state);
    return;
  }

  if (type == "action") {
    std::lock_guard<std::mutex> lock(stateMutex_);
    auto it = connections_.find(&conn);
    if (it == connections_.end() || it->second.roomCode.empty()) {
      sendError(conn, "no_room", "hello must be sent before action"); return;
    }
    const auto& state = it->second;
    if (!state.seatIndex || state.sessionToken.empty()) {
      sendError(conn, "spectator", "Spectators cannot submit actions"); return;
    }
    const int expectedVersion = message.value("expectedVersion", 0);
    if (!message.contains("action")) { sendError(conn, "missing_action", "Missing action payload"); return; }
    const auto parsed = codec::legalActionFromJson(message.at("action"));
    if (!parsed) { sendError(conn, "bad_action", "Action payload could not be parsed"); return; }
    const auto result = manager_.submitHumanAction(state.roomCode, *state.seatIndex, state.sessionToken, expectedVersion, *parsed);
    if (!result.ok) {
      sendError(conn, result.code, result.message);
      // If the failure was due to session takeover, drop the seat binding so
      // we don't keep submitting stale-token actions and so any UI logic that
      // reacts to spectator demotion works correctly.
      if (result.code == "session_invalidated") {
        it->second.seatIndex.reset();
        it->second.sessionToken.clear();
      }
      // Still resync the connection so it gets the latest snapshot/version.
      sendSnapshotLocked(conn, it->second);
      return;
    }
    manager_.noteSeatActivity(state.roomCode, *state.seatIndex);
    broadcastRoom(state.roomCode);
    aiWorkerCv_.notify_all();
    return;
  }

  if (type == "set_ai_delay") {
    std::lock_guard<std::mutex> lock(stateMutex_);
    auto it = connections_.find(&conn);
    if (it == connections_.end() || it->second.roomCode.empty()) {
      sendError(conn, "no_room", "hello must be sent before set_ai_delay"); return;
    }
    const auto& state = it->second;
    int delayMs = message.value("delayMs", 0);
    if (delayMs < 0) delayMs = 0;
    if (delayMs > 5000) delayMs = 5000;
    if (!manager_.setAiDelayMs(state.roomCode, delayMs)) {
      sendError(conn, "not_found", "Room does not exist"); return;
    }
    if (state.seatIndex) manager_.noteSeatActivity(state.roomCode, *state.seatIndex);
    broadcastRoom(state.roomCode);
    aiWorkerCv_.notify_all();
    return;
  }

  if (type == "set_auto_pass") {
    std::lock_guard<std::mutex> lock(stateMutex_);
    auto it = connections_.find(&conn);
    if (it == connections_.end() || it->second.roomCode.empty()) {
      sendError(conn, "no_room", "hello must be sent before set_auto_pass"); return;
    }
    const auto& state = it->second;
    const bool value = message.value("value", false);
    if (!manager_.setAutoPass(state.roomCode, value)) {
      sendError(conn, "not_found", "Room does not exist"); return;
    }
    if (state.seatIndex) manager_.noteSeatActivity(state.roomCode, *state.seatIndex);
    broadcastRoom(state.roomCode);
    return;
  }

  if (type == "set_min_fan") {
    std::lock_guard<std::mutex> lock(stateMutex_);
    auto it = connections_.find(&conn);
    if (it == connections_.end() || it->second.roomCode.empty()) {
      sendError(conn, "no_room", "hello must be sent before set_min_fan"); return;
    }
    const auto& state = it->second;
    int minFan = message.value("value", 3);
    if (minFan < 0) minFan = 0;
    if (minFan > 13) minFan = 13;
    if (!manager_.setMinFan(state.roomCode, minFan)) {
      sendError(conn, "not_found", "Room does not exist"); return;
    }
    broadcastRoom(state.roomCode);
    return;
  }

  sendError(conn, "unknown_type", "Unknown message type: " + type);
}

void WebServer::sendSnapshotLocked(crow::websocket::connection& conn, const ConnectionState& state) const {
  auto room = manager_.getRoom(state.roomCode);
  if (!room) return;
  std::optional<int> viewer = state.seatIndex;
  // Spectators don't see anyone's concealed tiles unless the round is finished.
  const auto snapshot = manager_.createSnapshot(*room, viewer);
  json payload{
      {"type", "snapshot"},
      {"snapshot", codec::toJson(snapshot)},
  };
  conn.send_text(payload.dump());
}

void WebServer::sendError(crow::websocket::connection& conn, const std::string& code, const std::string& message) const {
  json payload{
      {"type", "error"},
      {"code", code},
      {"message", message},
  };
  conn.send_text(payload.dump());
}

void WebServer::broadcastRoom(const std::string& roomCode) {
  const auto it = roomConnections_.find(roomCode);
  if (it == roomConnections_.end()) return;
  for (auto* conn : it->second) {
    const auto cit = connections_.find(conn);
    if (cit == connections_.end()) continue;
    sendSnapshotLocked(*conn, cit->second);
  }
}

} // namespace mahjong::server
