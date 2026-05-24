#include "server/room_server.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace mahjong::server {
namespace {

std::string randomToken(std::size_t bytes = 18) {
  static std::random_device device;
  static std::mt19937_64 generator(device());
  std::uniform_int_distribution<int> distribution(0, 255);
  std::ostringstream out;
  for (std::size_t index = 0; index < bytes; ++index) {
    out << std::hex << std::setw(2) << std::setfill('0') << distribution(generator);
  }
  return out.str();
}

std::string hashToken(const std::string& token) {
  uint64_t hash = 14695981039346656037ull;
  for (const unsigned char byte : token) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << std::hex << hash;
  return out.str();
}

std::string createRoomCode() {
  static constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  static std::random_device device;
  static std::mt19937 generator(device());
  std::uniform_int_distribution<std::size_t> distribution(0, sizeof(alphabet) - 2);
  std::string code;
  for (int index = 0; index < 6; ++index) code.push_back(alphabet[distribution(generator)]);
  return code;
}

bool isSessionAuthorized(const RoomSeatRecord& seat, const std::string& sessionToken) {
  return seat.controller == Controller::Human && seat.sessionTokenHash && *seat.sessionTokenHash == hashToken(sessionToken);
}

bool hasPassed(const RoomRecord& room, int seatIndex) {
  return std::find(room.pendingClaimPasses.begin(), room.pendingClaimPasses.end(), seatIndex) != room.pendingClaimPasses.end();
}

RoundState updateRoundPlayerIdentity(RoundState state, const RoomSeatRecord& seat) {
  auto& player = state.players[static_cast<std::size_t>(seat.seatIndex)];
  player.controller = seat.controller;
  player.displayName = seat.displayName;
  return state;
}

bool containsAction(const std::vector<LegalAction>& actions, const LegalAction& action) {
  return std::any_of(actions.begin(), actions.end(), [&](const LegalAction& candidate) { return candidate == action; });
}

} // namespace

std::string normalizeRoomCode(std::string roomCode) {
  roomCode.erase(std::remove_if(roomCode.begin(), roomCode.end(), [](unsigned char ch) { return std::isspace(ch); }), roomCode.end());
  std::transform(roomCode.begin(), roomCode.end(), roomCode.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
  return roomCode;
}

std::string actionLabel(const LegalAction& action) {
  if (action.type != ActionType::Kong) return toString(action.type);
  if (action.kongType == KongType::Concealed) return "concealed kong";
  if (action.kongType == KongType::Added) return "added kong";
  return "exposed kong";
}

RoomManager::RoomManager(std::string publicBaseUrl) : publicBaseUrl_(std::move(publicBaseUrl)) {}

CreateRoomResult RoomManager::createRoom(const std::string& seed) {
  std::string roomCode = createRoomCode();
  while (rooms_.contains(roomCode)) roomCode = createRoomCode();
  const auto round = createInitialRoundState(seed.empty() ? roomCode : seed);
  std::vector<std::string> tokens{randomToken(), randomToken(), randomToken(), randomToken()};
  RoomRecord room;
  room.roomCode = roomCode;
  room.version = 1;
  room.roundState = round;
  for (const auto& player : round.players) {
    room.seats.push_back({
      player.seatIndex,
      player.wind,
      Controller::Ai,
      player.displayName,
      hashToken(tokens[static_cast<std::size_t>(player.seatIndex)]),
      std::nullopt,
      false
    });
  }
  room.lastActivityAt = std::chrono::steady_clock::now();
  room.minFan = round.rules.minFan;
  rooms_[roomCode] = room;
  CreateRoomResult result;
  result.room = room;
  for (int seat = 0; seat < 4; ++seat) {
    result.claimLinks.push_back({seat, tokens[static_cast<std::size_t>(seat)], publicBaseUrl_ + "/claim?room=" + roomCode + "&seat=" + std::to_string(seat) + "&token=" + tokens[static_cast<std::size_t>(seat)]});
  }
  return result;
}

std::optional<RoomRecord> RoomManager::getRoom(const std::string& roomCode) const {
  const auto normalized = normalizeRoomCode(roomCode);
  const auto it = rooms_.find(normalized);
  return it == rooms_.end() ? std::nullopt : std::optional<RoomRecord>{it->second};
}

std::vector<RoomRecord> RoomManager::listRooms() const {
  std::vector<RoomRecord> rooms;
  for (const auto& [_, room] : rooms_) rooms.push_back(room);
  return rooms;
}

ClaimSeatResult RoomManager::claimSeat(const std::string& roomCode, int seatIndex, const std::string& claimToken, const std::string& displayName, bool forceTakeover) {
  const auto normalized = normalizeRoomCode(roomCode);
  auto it = rooms_.find(normalized);
  if (it == rooms_.end()) throw std::runtime_error("Room not found");
  if (seatIndex < 0 || seatIndex >= 4) throw std::runtime_error("Invalid seat index");
  auto& room = it->second;
  auto& seat = room.seats[static_cast<std::size_t>(seatIndex)];
  if (seat.claimTokenHash != hashToken(claimToken)) throw std::runtime_error("Invalid private seat claim token");
  // Active-seat guard: refuse to silently displace a live human player. A seat
  // is "actively held" when at least one live WebSocket is bound to it AND we
  // saw owner activity within the last few seconds (so a frozen background
  // tab whose timers stopped firing is NOT considered active and can be
  // recovered without confirmation). The caller can override by passing
  // forceTakeover=true (the frontend asks the user for explicit confirm).
  if (!forceTakeover && seat.controller == Controller::Human && seat.liveConnectionCount > 0) {
    const auto now = std::chrono::steady_clock::now();
    const auto grace = std::chrono::seconds(5);
    if (seat.lastSeatActivityAt.time_since_epoch().count() > 0 &&
        now - seat.lastSeatActivityAt < grace) {
      throw SeatInUseError("Seat is currently held by another player. Confirm takeover to override.");
    }
  }
  const auto displaced = seat.sessionTokenHash;
  const auto sessionToken = randomToken();
  seat.controller = Controller::Human;
  seat.displayName = displayName.empty() ? "Player " + std::to_string(seatIndex + 1) : displayName;
  seat.sessionTokenHash = hashToken(sessionToken);
  // Reset connection and idle state: the new owner is reconnecting from
  // scratch. The WebServer will bump liveConnectionCount on the subsequent
  // hello. We also reset the pending-action deadline so the new owner gets a
  // full grace window before the idle worker can act for them.
  seat.liveConnectionCount = 0;
  seat.connected = false;
  seat.lastSeatActivityAt = std::chrono::steady_clock::now();
  seat.pendingActionSince = std::chrono::steady_clock::time_point{};
  seat.pendingActionVersion = -1;
  room.roundState = updateRoundPlayerIdentity(room.roundState, seat);
  room.version += 1;
  room.lastActivityAt = std::chrono::steady_clock::now();
  if (room.aiDelayMs <= 0) scheduleAiForRoomSync(room);
  else refreshAiPending(room);
  return {room, sessionToken, displaced};
}

RoomSnapshot RoomManager::createSnapshot(const RoomRecord& room, std::optional<int> viewerSeatIndex) const {
  RoomSnapshot snapshot;
  snapshot.roomCode = room.roomCode;
  snapshot.version = room.version;
  snapshot.seats = room.seats;
  snapshot.phase = room.roundState.phase;
  snapshot.dealerSeat = room.roundState.dealerSeat;
  snapshot.prevailingWind = room.roundState.prevailingWind;
  snapshot.currentTurn = room.roundState.currentTurn;
  snapshot.turnNumber = room.roundState.turnNumber;
  snapshot.liveWallCount = static_cast<int>(room.roundState.wall.liveWall.size());
  snapshot.deadWallCount = static_cast<int>(room.roundState.wall.deadWall.size());
  snapshot.replacementDrawCount = static_cast<int>(room.roundState.wall.replacementDraws.size());
  snapshot.aiDelayMs = room.aiDelayMs;
  snapshot.autoPass = room.autoPass;
  snapshot.minFan = room.minFan;
  snapshot.winHistory = room.winHistory;
  snapshot.lastDiscard = room.roundState.lastDiscard;
  snapshot.lastDraw = room.roundState.lastDraw;
  snapshot.conclusion = room.roundState.conclusion;
  snapshot.viewerSeatIndex = viewerSeatIndex;
  const bool revealAll = room.roundState.phase == Phase::Finished;
  for (const auto& player : room.roundState.players) {
    PublicPlayerSnapshot publicPlayer;
    publicPlayer.seatIndex = player.seatIndex;
    publicPlayer.wind = player.wind;
    publicPlayer.controller = player.controller;
    publicPlayer.displayName = player.displayName;
    publicPlayer.score = player.score;
    publicPlayer.concealedCount = static_cast<int>(player.concealedTiles.size());
    if (revealAll || (viewerSeatIndex && *viewerSeatIndex == player.seatIndex)) publicPlayer.concealedTiles = player.concealedTiles;
    publicPlayer.flowers = player.flowers;
    publicPlayer.melds = player.melds;
    publicPlayer.discards = player.discards;
    snapshot.players.push_back(publicPlayer);
  }
  snapshot.legalActions = viewerSeatIndex ? roomLegalActions(room, *viewerSeatIndex) : std::vector<LegalAction>{};
  return snapshot;
}

SubmitActionResult RoomManager::submitHumanAction(const std::string& roomCode, int seatIndex, const std::string& sessionToken, int expectedVersion, const LegalAction& action) {
  return submitSeatAction(roomCode, seatIndex, sessionToken, expectedVersion, action, Controller::Human);
}

SubmitActionResult RoomManager::submitAiAction(const std::string& roomCode, int seatIndex, int expectedVersion, const LegalAction& action) {
  return submitSeatAction(roomCode, seatIndex, std::nullopt, expectedVersion, action, Controller::Ai);
}

std::vector<LegalAction> RoomManager::roomLegalActions(const RoomRecord& room, int seatIndex) const {
  if (room.roundState.phase == Phase::AwaitingClaims && hasPassed(room, seatIndex)) return {};
  return getLegalActions(room.roundState, seatIndex);
}

SubmitActionResult RoomManager::submitSeatAction(const std::string& roomCode, int seatIndex, std::optional<std::string> sessionToken, int expectedVersion, const LegalAction& action, Controller actor) {
  const auto normalized = normalizeRoomCode(roomCode);
  auto it = rooms_.find(normalized);
  if (it == rooms_.end()) return {false, "not_found", "Room not found", std::nullopt};
  auto& room = it->second;
  if (seatIndex < 0 || seatIndex >= 4) return {false, "unauthorized", "Invalid seat", room};
  const auto& seat = room.seats[static_cast<std::size_t>(seatIndex)];
  // Authorization rules:
  //   - actor=Human, sessionToken set        -> normal browser-initiated action: validate token.
  //   - actor=Human, sessionToken == nullopt -> internal idle-recovery action submitted by
  //                                             tickIdleHumans on behalf of an absent player;
  //                                             bypasses session check (the worker is trusted).
  //   - actor=Ai                             -> the seat must currently be Ai-controlled.
  if (actor == Controller::Human && sessionToken && !isSessionAuthorized(seat, *sessionToken)) return {false, "session_invalidated", "Seat session no longer valid", room};
  if (actor == Controller::Ai && seat.controller != Controller::Ai) return {false, "unauthorized", "AI hook cannot act for a human seat", room};
  if (expectedVersion != room.version) return {false, "stale_version", "Stale room version", room};
  const auto legalActions = roomLegalActions(room, seatIndex);
  if (!containsAction(legalActions, action)) return {false, "illegal_action", "Illegal " + actionLabel(action), room};
  // Claim-precedence gate: when a seat submits a Chow / Pong / exposed Kong
  // during a claim window, refuse it if any other not-yet-passed seat has a
  // higher-priority claim available on this same discard. This makes Win
  // trump Pong/Kong, and Pong/Kong trump Chow — matching Mahjong Soul-style
  // arbitration even though humans submit asynchronously rather than via a
  // simultaneous-vote window. Lower-priority claimants must wait for the
  // higher-priority seat to either claim or pass before they can act.
  if (room.roundState.phase == Phase::AwaitingClaims && room.roundState.lastDiscard &&
      claimPriority(action) > 0) {
    const int myPriority = claimPriority(action);
    const int discardSeat = room.roundState.lastDiscard->bySeat;
    for (int otherSeat = 0; otherSeat < 4; ++otherSeat) {
      if (otherSeat == seatIndex || otherSeat == discardSeat) continue;
      if (hasPassed(room, otherSeat)) continue;
      const auto otherActions = getLegalActions(room.roundState, otherSeat);
      for (const auto& opt : otherActions) {
        if (claimPriority(opt) > myPriority) {
          return {false, "claim_pending",
                  "Another seat has a higher-priority claim available; waiting for them to claim or pass",
                  room};
        }
      }
    }
  }
  const auto previousPhase = room.roundState.phase;
  auto nextRound = applyLegalAction(room, seatIndex, action);
  for (auto& nextSeat : room.seats) {
    nextSeat.wind = nextRound.players[static_cast<std::size_t>(nextSeat.seatIndex)].wind;
  }
  if (nextRound.phase == Phase::AwaitingClaims && action.type == ActionType::Pass) {
    if (!hasPassed(room, seatIndex)) room.pendingClaimPasses.push_back(seatIndex);
  } else if (nextRound.phase == Phase::AwaitingClaims) {
    room.pendingClaimPasses.clear();
  } else {
    room.pendingClaimPasses.clear();
  }
  // Capture a finalized round into the room's append-only history exactly
  // once, on the transition from any non-Finished phase into Finished.
  // NextRound starts a fresh state from a Finished base — we deliberately
  // ignore that case (previousPhase == Finished) so re-finalization does not
  // duplicate the entry.
  if (previousPhase != Phase::Finished && nextRound.phase == Phase::Finished && nextRound.conclusion) {
    room.winHistory.push_back(*nextRound.conclusion);
  }
  room.roundState = nextRound;
  room.version += 1;
  room.lastActivityAt = std::chrono::steady_clock::now();
  if (room.aiDelayMs <= 0) {
    scheduleAiForRoomSync(room);
  } else {
    refreshAiPending(room);
  }
  return {true, "ok", "Action accepted", room};
}

RoundState RoomManager::applyLegalAction(const RoomRecord& room, int seatIndex, const LegalAction& action) const {
  if (action.type == ActionType::NextRound) return createNextRoundState(room.roundState);
  if (action.type == ActionType::Draw) return drawTile(room.roundState, seatIndex);
  if (action.type == ActionType::Discard) return discardTile(room.roundState, action.tileId);
  if (action.type == ActionType::Pass) {
    if (room.roundState.phase != Phase::AwaitingClaims || !room.roundState.lastDiscard) return room.roundState;
    std::vector<int> passers = room.pendingClaimPasses;
    if (std::find(passers.begin(), passers.end(), seatIndex) == passers.end()) passers.push_back(seatIndex);
    const int discardSeat = room.roundState.lastDiscard->bySeat;
    bool allPassed = true;
    for (int candidate = 0; candidate < 4; ++candidate) {
      if (candidate == discardSeat) continue;
      allPassed = allPassed && std::find(passers.begin(), passers.end(), candidate) != passers.end();
    }
    return allPassed ? passClaimWindow(room.roundState) : room.roundState;
  }
  if (action.type == ActionType::Win) return action.source == WinSource::Discard ? claimDiscard(room.roundState, seatIndex, action) : declareSelfDrawWin(room.roundState);
  if (action.type == ActionType::Kong && action.kongType != KongType::Exposed) return declareKong(room.roundState, action.tileKey, action.meldId);
  return claimDiscard(room.roundState, seatIndex, action);
}

void RoomManager::scheduleAiForRoomSync(RoomRecord& room) {
  if (room.roundState.phase == Phase::Finished) { room.aiPending = false; return; }
  if (std::none_of(room.seats.begin(), room.seats.end(), [](const RoomSeatRecord& seat) { return seat.controller == Controller::Human; })) { room.aiPending = false; return; }
  bool progressed = true;
  int guard = 0;
  while (progressed && guard++ < 16 && room.roundState.phase != Phase::Finished) {
    progressed = false;
    for (const auto& seat : room.seats) {
      if (seat.controller != Controller::Ai) continue;
      const auto actions = roomLegalActions(room, seat.seatIndex);
      if (actions.empty()) continue;
      const auto action = selectAiAction(room.roundState, seat.seatIndex, actions, AiDifficulty::Medium, room.roomCode + ":" + std::to_string(room.version));
      auto result = submitAiAction(room.roomCode, seat.seatIndex, room.version, action);
      if (result.ok && result.room) {
        room = *result.room;
        progressed = true;
        break;
      }
    }
  }
  refreshAiPending(room);
}

void RoomManager::refreshAiPending(RoomRecord& room) {
  if (room.roundState.phase == Phase::Finished) { room.aiPending = false; return; }
  if (std::none_of(room.seats.begin(), room.seats.end(), [](const RoomSeatRecord& seat) { return seat.controller == Controller::Human; })) {
    // No humans yet — let AI run synchronously when a human eventually joins.
    room.aiPending = false;
    return;
  }
  // Does any AI seat have legal actions to take right now?
  bool aiHasWork = false;
  for (const auto& seat : room.seats) {
    if (seat.controller != Controller::Ai) continue;
    if (!roomLegalActions(room, seat.seatIndex).empty()) { aiHasWork = true; break; }
  }
  if (aiHasWork) {
    room.aiPending = true;
    room.nextAiAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, room.aiDelayMs));
  } else {
    room.aiPending = false;
  }
}

bool RoomManager::setAiDelayMs(const std::string& roomCode, int delayMs) {
  const auto normalized = normalizeRoomCode(roomCode);
  auto it = rooms_.find(normalized);
  if (it == rooms_.end()) return false;
  const int clamped = std::max(0, std::min(5000, delayMs));
  auto& room = it->second;
  room.aiDelayMs = clamped;
  room.lastActivityAt = std::chrono::steady_clock::now();
  // Reschedule pending AI with the new delay.
  if (clamped > 0) refreshAiPending(room);
  else if (room.aiPending) {
    // Delay just dropped to 0 — flush the AI cascade now.
    scheduleAiForRoomSync(room);
  }
  return true;
}

int RoomManager::getAiDelayMs(const std::string& roomCode) const {
  const auto normalized = normalizeRoomCode(roomCode);
  const auto it = rooms_.find(normalized);
  return it == rooms_.end() ? 0 : it->second.aiDelayMs;
}

bool RoomManager::setAutoPass(const std::string& roomCode, bool value) {
  const auto normalized = normalizeRoomCode(roomCode);
  auto it = rooms_.find(normalized);
  if (it == rooms_.end()) return false;
  auto& room = it->second;
  if (room.autoPass != value) {
    room.autoPass = value;
    // Bump version so every connected client sees the snapshot diff and
    // updates its toggle.
    room.version += 1;
    room.lastActivityAt = std::chrono::steady_clock::now();
  }
  return true;
}

bool RoomManager::getAutoPass(const std::string& roomCode) const {
  const auto normalized = normalizeRoomCode(roomCode);
  const auto it = rooms_.find(normalized);
  return it != rooms_.end() && it->second.autoPass;
}

bool RoomManager::setMinFan(const std::string& roomCode, int minFan) {
  const auto normalized = normalizeRoomCode(roomCode);
  auto it = rooms_.find(normalized);
  if (it == rooms_.end()) return false;
  const int clamped = std::max(0, std::min(13, minFan));
  auto& room = it->second;
  if (room.minFan != clamped || room.roundState.rules.minFan != clamped) {
    room.minFan = clamped;
    // Mutate the live round's rules so the change takes effect immediately
    // (e.g., a settlement scored right after the toggle uses the new value).
    room.roundState.rules.minFan = clamped;
    room.version += 1;
    room.lastActivityAt = std::chrono::steady_clock::now();
  }
  return true;
}

int RoomManager::getMinFan(const std::string& roomCode) const {
  const auto normalized = normalizeRoomCode(roomCode);
  const auto it = rooms_.find(normalized);
  return it == rooms_.end() ? 3 : it->second.minFan;
}

std::vector<RoundConclusion> RoomManager::getWinHistory(const std::string& roomCode) const {
  const auto normalized = normalizeRoomCode(roomCode);
  const auto it = rooms_.find(normalized);
  if (it == rooms_.end()) return {};
  return it->second.winHistory;
}

bool RoomManager::tickAi(const std::string& roomCode) {
  const auto normalized = normalizeRoomCode(roomCode);
  auto it = rooms_.find(normalized);
  if (it == rooms_.end()) return false;
  auto& room = it->second;
  if (!room.aiPending) return false;
  if (room.roundState.phase == Phase::Finished) { room.aiPending = false; return false; }
  for (const auto& seat : room.seats) {
    if (seat.controller != Controller::Ai) continue;
    const auto actions = roomLegalActions(room, seat.seatIndex);
    if (actions.empty()) continue;
    const auto action = selectAiAction(room.roundState, seat.seatIndex, actions, AiDifficulty::Medium, room.roomCode + ":" + std::to_string(room.version));
    auto result = submitAiAction(room.roomCode, seat.seatIndex, room.version, action);
    if (result.ok && result.room) {
      // submitAiAction -> submitSeatAction -> refreshAiPending already ran,
      // and submitSeatAction already bumped lastActivityAt.
      return true;
    }
  }
  room.aiPending = false;
  return false;
}

std::optional<std::chrono::steady_clock::time_point> RoomManager::nextAiDueAt(const std::string& roomCode) const {
  const auto normalized = normalizeRoomCode(roomCode);
  const auto it = rooms_.find(normalized);
  if (it == rooms_.end()) return std::nullopt;
  if (!it->second.aiPending) return std::nullopt;
  return it->second.nextAiAt;
}

void RoomManager::touchRoom(const std::string& roomCode) {
  const auto normalized = normalizeRoomCode(roomCode);
  auto it = rooms_.find(normalized);
  if (it == rooms_.end()) return;
  it->second.lastActivityAt = std::chrono::steady_clock::now();
}

std::vector<std::string> RoomManager::evictIdleRooms(
    std::chrono::steady_clock::time_point now,
    std::chrono::seconds ttlFinished,
    std::chrono::seconds ttlActive,
    std::size_t maxRooms) {
  std::vector<std::string> evicted;

  // Pass 1: TTL-based eviction. Rooms whose idle duration exceeds the
  // phase-specific TTL get marked for removal.
  for (auto it = rooms_.begin(); it != rooms_.end(); ) {
    const auto& room = it->second;
    const auto ttl = (room.roundState.phase == Phase::Finished) ? ttlFinished : ttlActive;
    if (now - room.lastActivityAt > ttl) {
      evicted.push_back(it->first);
      it = rooms_.erase(it);
    } else {
      ++it;
    }
  }

  // Pass 2: hard cap. If still over maxRooms (and a cap is set), drop the
  // oldest-idle rooms regardless of phase. Finished rooms sort first within
  // each tier of lastActivityAt because they're cheaper to lose.
  if (maxRooms > 0 && rooms_.size() > maxRooms) {
    struct Candidate {
      std::string code;
      std::chrono::steady_clock::time_point lastActivityAt;
      bool finished;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(rooms_.size());
    for (const auto& [code, room] : rooms_) {
      candidates.push_back({code, room.lastActivityAt, room.roundState.phase == Phase::Finished});
    }
    // Sort: oldest-idle first, finished-first as a tie-breaker.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
      if (a.lastActivityAt != b.lastActivityAt) return a.lastActivityAt < b.lastActivityAt;
      return a.finished && !b.finished;
    });
    const std::size_t toEvict = rooms_.size() - maxRooms;
    for (std::size_t i = 0; i < toEvict && i < candidates.size(); ++i) {
      evicted.push_back(candidates[i].code);
      rooms_.erase(candidates[i].code);
    }
  }

  return evicted;
}

// ===================== Per-seat liveness =====================

std::vector<std::string> RoomManager::listRoomCodes() const {
  std::vector<std::string> codes;
  codes.reserve(rooms_.size());
  for (const auto& [code, _room] : rooms_) codes.push_back(code);
  return codes;
}

void RoomManager::setSeatConnectionCount(const std::string& roomCode, int seatIndex, int count) {
  const auto normalized = normalizeRoomCode(roomCode);
  auto it = rooms_.find(normalized);
  if (it == rooms_.end()) return;
  if (seatIndex < 0 || seatIndex >= 4) return;
  auto& seat = it->second.seats[static_cast<std::size_t>(seatIndex)];
  if (count < 0) count = 0;
  const bool wasConnected = seat.connected;
  seat.liveConnectionCount = count;
  seat.connected = count > 0;
  // When all owners disconnect we DO NOT clear pendingActionSince: leaving the
  // deadline ticking lets the idle worker auto-pass for them shortly. When
  // they reconnect we DO refresh activity so the idle deadline resets and
  // they get a fresh thinking window.
  if (seat.connected && !wasConnected) {
    seat.lastSeatActivityAt = std::chrono::steady_clock::now();
  }
  it->second.lastActivityAt = std::chrono::steady_clock::now();
}

void RoomManager::noteSeatActivity(const std::string& roomCode, int seatIndex) {
  const auto normalized = normalizeRoomCode(roomCode);
  auto it = rooms_.find(normalized);
  if (it == rooms_.end()) return;
  if (seatIndex < 0 || seatIndex >= 4) return;
  auto& seat = it->second.seats[static_cast<std::size_t>(seatIndex)];
  seat.lastSeatActivityAt = std::chrono::steady_clock::now();
  it->second.lastActivityAt = seat.lastSeatActivityAt;
}

bool RoomManager::isSeatActivelyHeld(const std::string& roomCode, int seatIndex,
                                     std::chrono::steady_clock::time_point now,
                                     std::chrono::milliseconds graceMs) const {
  const auto normalized = normalizeRoomCode(roomCode);
  const auto it = rooms_.find(normalized);
  if (it == rooms_.end()) return false;
  if (seatIndex < 0 || seatIndex >= 4) return false;
  const auto& seat = it->second.seats[static_cast<std::size_t>(seatIndex)];
  if (seat.controller != Controller::Human) return false;
  if (seat.liveConnectionCount <= 0) return false;
  if (seat.lastSeatActivityAt.time_since_epoch().count() == 0) return false;
  return now - seat.lastSeatActivityAt < graceMs;
}

// ===================== Idle-seat resilience =====================

namespace {

// Picks a "safe" recovery action for a human seat that has gone silent past
// the idleActMs deadline. Order:
//   1. Pass (during a claim window): the most conservative choice -- it
//      simply declines any call option (chow/pong/kong/win) so the human
//      doesn't make a binding strategic decision while they're away.
//   2. If pass is not legal (e.g., it's the seat's own turn and they must
//      draw or discard), fall back to selectAiAction so the round can still
//      advance. This is rarer because most soft-locks come from pass-only
//      claim windows.
LegalAction pickIdleAction(const RoundState& state, int seatIndex,
                           const std::vector<LegalAction>& legalActions,
                           const std::string& aiSeed) {
  for (const auto& action : legalActions) {
    if (action.type == ActionType::Pass) return action;
  }
  return selectAiAction(state, seatIndex, legalActions, AiDifficulty::Medium, aiSeed);
}

}  // namespace

RoomManager::IdleTickOutcome RoomManager::tickIdleHumans(
    const std::string& roomCode,
    std::chrono::milliseconds idleActMs,
    std::chrono::milliseconds idleTakeoverMs) {
  IdleTickOutcome outcome;
  const auto normalized = normalizeRoomCode(roomCode);
  auto it = rooms_.find(normalized);
  if (it == rooms_.end()) return outcome;
  auto& room = it->second;
  if (room.roundState.phase == Phase::Finished) {
    // Clear pending markers so a fresh round starts with a clean grace.
    for (auto& seat : room.seats) {
      seat.pendingActionSince = std::chrono::steady_clock::time_point{};
      seat.pendingActionVersion = -1;
    }
    return outcome;
  }

  const auto now = std::chrono::steady_clock::now();
  bool roomChanged = false;

  // We may submit on behalf of a seat which itself mutates the room. Re-scan
  // each iteration in case the version moved.
  bool progressed = true;
  int guard = 0;
  while (progressed && guard++ < 16) {
    progressed = false;
    for (auto& seat : room.seats) {
      if (seat.controller != Controller::Human) continue;

      // Full takeover threshold (checked FIRST, independent of any pending
      // action markers). If the seat has been disconnected (no live WS) for
      // longer than `idleTakeoverMs`, convert it to AI so the room can keep
      // playing for the rest of the round/session without that player. The
      // displaced human can later reclaim via the regular HTTP claim flow
      // which forces controller back to Human and issues a fresh session
      // token. A connected-but-lurking human is handled by repeated
      // auto-pass below; that's fine -- the round keeps progressing and we
      // don't need to seize their seat.
      const auto disconnectedFor = now - seat.lastSeatActivityAt;
      const bool disconnectedTooLong =
          seat.liveConnectionCount <= 0 &&
          seat.lastSeatActivityAt.time_since_epoch().count() > 0 &&
          disconnectedFor >= idleTakeoverMs;
      if (disconnectedTooLong) {
        seat.controller = Controller::Ai;
        seat.sessionTokenHash.reset();
        seat.liveConnectionCount = 0;
        seat.connected = false;
        seat.pendingActionSince = std::chrono::steady_clock::time_point{};
        seat.pendingActionVersion = -1;
        outcome.seatsConvertedToAi.push_back(seat.seatIndex);
        room.version += 1;
        room.lastActivityAt = now;
        if (room.aiDelayMs <= 0) scheduleAiForRoomSync(room);
        else refreshAiPending(room);
        roomChanged = true;
        progressed = true;
        break;
      }

      auto actions = roomLegalActions(room, seat.seatIndex);
      if (actions.empty()) {
        // Not blocking anything -- reset the pending markers so the next time
        // a claim window opens they get a fresh deadline.
        seat.pendingActionSince = std::chrono::steady_clock::time_point{};
        seat.pendingActionVersion = -1;
        continue;
      }
      // First time we see legal actions for this seat at this version: start
      // the deadline now.
      if (seat.pendingActionVersion != room.version) {
        seat.pendingActionVersion = room.version;
        seat.pendingActionSince = now;
        continue;
      }
      const auto elapsed = now - seat.pendingActionSince;

      // Auto-act threshold: prefer Pass to avoid making any binding decision
      // on the absent player's behalf. Falls back to AI choice if no pass is
      // legal (it's the seat's own turn and the round still needs an action).
      if (elapsed >= idleActMs) {
        const auto action = pickIdleAction(
            room.roundState, seat.seatIndex, actions,
            room.roomCode + ":idle:" + std::to_string(room.version));
        const auto result = submitSeatAction(
            room.roomCode, seat.seatIndex, std::nullopt, room.version,
            action, Controller::Human);
        if (result.ok && result.room) {
          room = *result.room;
          roomChanged = true;
          progressed = true;
          break;
        }
      }
    }
  }

  outcome.changed = roomChanged;
  return outcome;
}

std::optional<std::chrono::steady_clock::time_point> RoomManager::nextIdleDueAt(
    const std::string& roomCode,
    std::chrono::milliseconds idleActMs,
    std::chrono::milliseconds idleTakeoverMs) const {
  const auto normalized = normalizeRoomCode(roomCode);
  const auto it = rooms_.find(normalized);
  if (it == rooms_.end()) return std::nullopt;
  const auto& room = it->second;
  if (room.roundState.phase == Phase::Finished) return std::nullopt;
  std::optional<std::chrono::steady_clock::time_point> earliest;
  for (const auto& seat : room.seats) {
    if (seat.controller != Controller::Human) continue;
    if (seat.pendingActionVersion != room.version) continue;
    if (seat.pendingActionSince.time_since_epoch().count() == 0) continue;
    const auto due = seat.pendingActionSince + std::min(idleActMs, idleTakeoverMs);
    if (!earliest || due < *earliest) earliest = due;
  }
  return earliest;
}

}  // namespace mahjong::server
