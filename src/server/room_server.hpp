#pragma once

#include "core/mahjong_core.hpp"

#include <chrono>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mahjong::server {

struct RoomSeatRecord {
  int seatIndex{};
  Wind wind{};
  Controller controller{Controller::Ai};
  std::string displayName;
  std::string claimTokenHash;
  std::optional<std::string> sessionTokenHash;
  // Number of live WebSocket connections currently bound to this seat with a
  // matching sessionToken. Maintained by WebServer via setSeatConnectionCount
  // on hello / close. >0 means at least one browser tab actively holds the
  // seat. Used by claimSeat's "seat in use" guard and by tickIdleHumans to
  // decide between auto-pass (still connected) and full AI takeover
  // (disconnected too long).
  int liveConnectionCount{0};
  // Convenience: true iff liveConnectionCount > 0. Mirrored into snapshots so
  // the lobby can show "Active" vs "Idle" badges.
  bool connected{};
  // Steady-clock timestamp of the last activity by this seat's owner over
  // WebSocket (hello, ping, action, set_auto_pass, set_ai_delay). Updated by
  // RoomManager::noteSeatActivity. Used as a tie-breaker by claimSeat's
  // takeover grace period.
  std::chrono::steady_clock::time_point lastSeatActivityAt{};
  // When the seat first had a non-empty legalActions list at the current room
  // version (i.e., became "blocking the game waiting for this seat's input").
  // Cleared when legalActions become empty or when the seat acts. Drives the
  // idle-act and idle-takeover deadlines independently of WS pings -- a tab
  // that is alive but frozen will still hit these deadlines because the
  // pendingAction was never resolved.
  std::chrono::steady_clock::time_point pendingActionSince{};
  // Room version at which pendingActionSince was last recomputed. When the
  // current room.version moves past this, the worker resets pendingActionSince
  // to "now" -- i.e., each new claim window or turn gets its own grace period.
  int pendingActionVersion{-1};
};

struct RoomRecord {
  std::string roomCode;
  int version{};
  std::vector<RoomSeatRecord> seats;
  RoundState roundState;
  std::vector<int> pendingClaimPasses;
  // Per-room AI think-time. 0 means "run the AI cascade synchronously inside
  // the human action that triggered it" (preserves the original behavior /
  // tests). Anything > 0 makes scheduleAiForRoom defer to the background AI
  // ticker so the human sees one snapshot per AI move with this delay.
  int aiDelayMs{0};
  // When the next pending AI action should be executed. Only meaningful when
  // aiDelayMs > 0 AND an AI seat has work to do.
  std::chrono::steady_clock::time_point nextAiAt{};
  bool aiPending{false};
  // Room-wide toggle: when true, browser clients in this room auto-pass on
  // any pass-only claim window. Persists across reconnects and is shared
  // across all clients in the room (last writer wins).
  bool autoPass{false};
  // Room-wide "Chicken Hand" minimum fan threshold. Mirrors the value in
  // roundState.rules.minFan so the setting persists across NextRound resets
  // and is broadcast to all clients via snapshot. Default 3 (standard HK
  // Mahjong); setting it to 0 enables Chicken Hand (any winning hand pays).
  int minFan{3};
  // Append-only log of finished round conclusions. Pushed each time the
  // round transitions to Finished; survives NextRound resets so players can
  // review the match history.
  std::vector<RoundConclusion> winHistory;
  // Steady-clock timestamp of the last meaningful activity in this room.
  // Updated on creation, seat claim, any submitted action, any AI tick, any
  // setting change (aiDelay/autoPass), and on any WebSocket message bound
  // to this room. Used by the cleanup worker to evict idle rooms.
  std::chrono::steady_clock::time_point lastActivityAt{std::chrono::steady_clock::now()};
};

struct ClaimLink {
  int seatIndex{};
  std::string token;
  std::string url;
};

struct CreateRoomResult {
  RoomRecord room;
  std::vector<ClaimLink> claimLinks;
};

struct ClaimSeatResult {
  RoomRecord room;
  std::string sessionToken;
  // Old session token hash that was just invalidated (if any). The WebServer
  // uses this to find existing WS connections to the same seat and push a
  // session_invalidated error before silently demoting them.
  std::optional<std::string> displacedSessionTokenHash;
};

// Thrown by RoomManager::claimSeat when a seat is currently held by a live
// WebSocket connection and the caller did not pass forceTakeover=true.
// The WebServer maps this to HTTP 409 with error code "seat_in_use".
class SeatInUseError : public std::runtime_error {
 public:
  explicit SeatInUseError(const std::string& msg) : std::runtime_error(msg) {}
};

struct PublicPlayerSnapshot {
  int seatIndex{};
  Wind wind{};
  Controller controller{Controller::Ai};
  std::string displayName;
  int score{};
  int concealedCount{};
  std::optional<std::vector<Tile>> concealedTiles;
  std::vector<Tile> flowers;
  std::vector<Meld> melds;
  std::vector<Tile> discards;
};

struct RoomSnapshot {
  std::string roomCode;
  int version{};
  std::vector<RoomSeatRecord> seats;
  Phase phase{};
  int dealerSeat{};
  Wind prevailingWind{};
  int currentTurn{};
  int turnNumber{};
  int liveWallCount{};
  int deadWallCount{};
  int replacementDrawCount{};
  int aiDelayMs{0};
  bool autoPass{false};
  int minFan{3};
  std::vector<RoundConclusion> winHistory;
  std::optional<LastDiscard> lastDiscard;
  std::optional<LastDraw> lastDraw;
  std::optional<RoundConclusion> conclusion;
  std::optional<int> viewerSeatIndex;
  std::vector<PublicPlayerSnapshot> players;
  std::vector<LegalAction> legalActions;
};

struct SubmitActionResult {
  bool ok{};
  std::string code;
  std::string message;
  std::optional<RoomRecord> room;
};

class RoomManager {
 public:
  explicit RoomManager(std::string publicBaseUrl = "local://mahjong");

  CreateRoomResult createRoom(const std::string& seed = "");
  std::optional<RoomRecord> getRoom(const std::string& roomCode) const;
  std::vector<RoomRecord> listRooms() const;
  ClaimSeatResult claimSeat(const std::string& roomCode, int seatIndex, const std::string& claimToken, const std::string& displayName, bool forceTakeover = false);
  RoomSnapshot createSnapshot(const RoomRecord& room, std::optional<int> viewerSeatIndex = std::nullopt) const;
  SubmitActionResult submitHumanAction(const std::string& roomCode, int seatIndex, const std::string& sessionToken, int expectedVersion, const LegalAction& action);
  SubmitActionResult submitAiAction(const std::string& roomCode, int seatIndex, int expectedVersion, const LegalAction& action);

  // Sets the AI think-time for a room. Clamped to [0, 5000] ms.
  // Returns true on success, false if the room does not exist.
  bool setAiDelayMs(const std::string& roomCode, int delayMs);
  // Returns the room's configured AI delay, or 0 if the room is missing.
  int getAiDelayMs(const std::string& roomCode) const;
  // Sets the room-wide autoPass toggle. Returns false if the room does not
  // exist. Idempotent — setting to the same value is a no-op.
  bool setAutoPass(const std::string& roomCode, bool value);
  // Reads the room's autoPass toggle, defaults to false if the room is
  // missing.
  bool getAutoPass(const std::string& roomCode) const;
  // Sets the room-wide minimum fan threshold (Chicken Hand control).
  // Clamped to [0, 13]. Updates both the per-room config and the active
  // round's rules so the change applies immediately. Returns false if the
  // room does not exist.
  bool setMinFan(const std::string& roomCode, int minFan);
  // Reads the room's current minFan, or 3 if the room is missing.
  int getMinFan(const std::string& roomCode) const;
  // Append-only round history: a copy of every RoundConclusion that was
  // finalized in this room, in order. Empty vector if no rounds have ended
  // yet or the room is missing.
  std::vector<RoundConclusion> getWinHistory(const std::string& roomCode) const;
  // Runs at most ONE pending AI action for the room. Returns true if an
  // action was applied (caller should broadcast). False if nothing to do.
  // Use this from a worker thread to advance AI when aiDelayMs > 0.
  bool tickAi(const std::string& roomCode);
  // Returns when the next AI tick is due for the room, or nullopt if there
  // is no pending AI work.
  std::optional<std::chrono::steady_clock::time_point> nextAiDueAt(const std::string& roomCode) const;

  // Bumps the room's lastActivityAt timestamp. No-op if the room is missing.
  // Use from WS handlers when any message arrives that is bound to a room.
  void touchRoom(const std::string& roomCode);

  // Returns how many rooms are currently in memory. For observability /
  // tests; does not lock.
  std::size_t roomCount() const { return rooms_.size(); }

  // Test-only: replaces (or inserts) a room record. Used by unit tests to
  // install contrived game states (e.g., specific claim-window scenarios)
  // without going through createRoom's deterministic deal. Production code
  // should never call this.
  void injectRoomForTest(const RoomRecord& room) { rooms_[room.roomCode] = room; }

  // ---------- Per-seat liveness tracking (called by WebServer) ----------

  // Lists the current room codes (a snapshot copy so callers can iterate
  // without holding the manager's lock if they don't need consistency).
  std::vector<std::string> listRoomCodes() const;

  // Sets the live WebSocket connection count for a given seat. Maintains
  // both seat.liveConnectionCount and the convenience seat.connected flag.
  // When the count drops to 0 the next tickIdleHumans pass can advance the
  // idle deadline; when it returns to >0 the seat is considered actively
  // owned again. No-op if the room or seat does not exist.
  void setSeatConnectionCount(const std::string& roomCode, int seatIndex, int count);

  // Bumps the seat's lastSeatActivityAt timestamp. Called by WebServer on
  // any inbound WS message bound to this seat (hello, action, ping, setting
  // change). Used as a tie-breaker by claimSeat's takeover grace window.
  // No-op if the room or seat does not exist.
  void noteSeatActivity(const std::string& roomCode, int seatIndex);

  // Returns true if the seat is currently considered actively held by a
  // live owner: liveConnectionCount > 0 AND the most recent seat activity
  // is newer than (now - graceMs). Used by WebServer's claimSeat handler to
  // decide whether to surface the seat_in_use 409 to the caller.
  // No-op (returns false) if the room or seat does not exist.
  bool isSeatActivelyHeld(const std::string& roomCode, int seatIndex,
                          std::chrono::steady_clock::time_point now,
                          std::chrono::milliseconds graceMs) const;

  // ---------- Idle-seat resilience (called by WebServer's AI worker) ----

  struct IdleTickOutcome {
    bool changed{false};
    // Seats that just had their controller flipped from Human to Ai (full
    // takeover). The caller should broadcast the room snapshot AND push a
    // session_invalidated error to any old WS connections holding that
    // seat so the displaced browser knows it was kicked.
    std::vector<int> seatsConvertedToAi;
  };

  // For each Human seat in `roomCode`, checks how long it has been blocking
  // on a pending decision (a non-empty legalActions list at the current
  // version). When the block exceeds idleActMs we submit a "pass" on the
  // seat's behalf (or, if pass is not legal, a deterministic AI action so
  // the seat's own turn can advance). When the block exceeds idleTakeoverMs
  // OR the seat has been disconnected for that long, we flip the seat's
  // controller to Ai so the normal AI worker resumes play for the rest of
  // the round; the human can later reclaim by re-using their seat token.
  //
  // Returns IdleTickOutcome.changed = true if anything mutated; the worker
  // should re-broadcast in that case.
  IdleTickOutcome tickIdleHumans(const std::string& roomCode,
                                 std::chrono::milliseconds idleActMs,
                                 std::chrono::milliseconds idleTakeoverMs);

  // Returns when the next idle deadline (auto-pass or takeover) is due in
  // this room, or nullopt if no human seat is currently blocking. Used by
  // the worker to size its wait_for window so idle recovery wakes up
  // promptly even when there are no AI moves pending.
  std::optional<std::chrono::steady_clock::time_point> nextIdleDueAt(
      const std::string& roomCode,
      std::chrono::milliseconds idleActMs,
      std::chrono::milliseconds idleTakeoverMs) const;

  // Evicts rooms that have been idle past their TTL, then if still over
  // maxRooms, evicts the oldest by lastActivityAt until at maxRooms.
  // - Rooms in Phase::Finished use ttlFinished.
  // - All other phases use ttlActive.
  // - maxRooms == 0 disables the size cap.
  // Returns the list of evicted room codes (caller closes any open WS
  // connections bound to them).
  std::vector<std::string> evictIdleRooms(
      std::chrono::steady_clock::time_point now,
      std::chrono::seconds ttlFinished,
      std::chrono::seconds ttlActive,
      std::size_t maxRooms);

 private:
  std::string publicBaseUrl_;
  std::map<std::string, RoomRecord> rooms_;

  std::vector<LegalAction> roomLegalActions(const RoomRecord& room, int seatIndex) const;
  SubmitActionResult submitSeatAction(const std::string& roomCode, int seatIndex, std::optional<std::string> sessionToken, int expectedVersion, const LegalAction& action, Controller actor);
  RoundState applyLegalAction(const RoomRecord& room, int seatIndex, const LegalAction& action) const;
  // Synchronous "cascade all AI moves" path. Only invoked when aiDelayMs == 0.
  void scheduleAiForRoomSync(RoomRecord& room);
  // Updates aiPending/nextAiAt based on whether any AI seat has legal actions.
  void refreshAiPending(RoomRecord& room);
};

std::string normalizeRoomCode(std::string roomCode);
std::string actionLabel(const LegalAction& action);

} // namespace mahjong::server
