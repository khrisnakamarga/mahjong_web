#include "core/mahjong_core.hpp"
#include "server/room_server.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::vector<mahjong::Tile> tiles(const std::vector<std::string>& keys) {
  std::set<std::string> used;
  std::vector<mahjong::Tile> result;
  auto set = mahjong::createTileSet();
  for (const auto& key : keys) {
    auto it = std::find_if(set.begin(), set.end(), [&](const mahjong::Tile& tile) {
      return tile.key == key && !used.contains(tile.id);
    });
    if (it == set.end()) throw std::runtime_error("Missing test tile " + key);
    used.insert(it->id);
    result.push_back(*it);
  }
  return result;
}

mahjong::PlayerState emptyPlayer(int seat, std::vector<mahjong::Tile> concealed = {}) {
  mahjong::PlayerState player;
  player.seatIndex = seat;
  player.wind = std::array<mahjong::Wind, 4>{mahjong::Wind::East, mahjong::Wind::South, mahjong::Wind::West, mahjong::Wind::North}[static_cast<std::size_t>(seat)];
  player.displayName = "P" + std::to_string(seat);
  player.concealedTiles = std::move(concealed);
  return player;
}

mahjong::RoundState roundState() {
  mahjong::RoundState state;
  state.phase = mahjong::Phase::AwaitingDiscard;
  state.dealerSeat = 0;
  state.prevailingWind = mahjong::Wind::East;
  state.currentTurn = 0;
  state.turnNumber = 1;
  state.wall.seed = "test";
  state.players = {emptyPlayer(0), emptyPlayer(1), emptyPlayer(2), emptyPlayer(3)};
  return state;
}

mahjong::Meld calledMeld(mahjong::MeldKind kind, const std::vector<std::string>& keys, int fromSeat) {
  mahjong::Meld meld;
  meld.kind = kind;
  meld.tiles = tiles(keys);
  meld.fromSeat = fromSeat;
  meld.concealed = false;
  return meld;
}

void testTilesAndWall() {
  const auto set = mahjong::createTileSet();
  std::set<std::string> ids;
  for (const auto& tile : set) ids.insert(tile.id);
  require(set.size() == 144, "tile set has 144 tiles");
  require(ids.size() == 144, "tile ids are unique");
  const auto first = mahjong::generateWall("seed-a");
  const auto second = mahjong::generateWall("seed-a");
  const auto third = mahjong::generateWall("seed-b");
  require(first.liveWall.size() == 130, "live wall count");
  require(first.deadWall.size() == 14, "dead wall count");
  require(first.liveWall.front().id == second.liveWall.front().id, "deterministic wall");
  require(first.liveWall.front().id != third.liveWall.front().id, "seed changes wall");
}

void testHandValidation() {
  require(mahjong::validateWinningHand(tiles({"dots-1", "dots-2", "dots-3", "bamboo-1", "bamboo-2", "bamboo-3", "characters-1", "characters-2", "characters-3", "east", "east", "east", "red", "red"})).kind == "standard", "standard win");
  require(mahjong::validateWinningHand(tiles({"dots-1", "dots-1", "dots-2", "dots-2", "dots-3", "dots-3", "dots-4", "dots-4", "dots-5", "dots-5", "dots-6", "dots-6", "dots-7", "dots-7"})).kind == "sevenPairs", "seven pairs");
  require(mahjong::validateWinningHand(tiles({"dots-1", "dots-9", "bamboo-1", "bamboo-9", "characters-1", "characters-9", "east", "south", "west", "north", "red", "green", "white", "red"})).kind == "thirteenOrphans", "thirteen orphans");
  require(mahjong::validateWinningHand(tiles({"dots-1", "dots-1", "dots-1", "dots-2", "dots-3", "dots-4", "dots-5", "dots-5", "dots-6", "dots-7", "dots-8", "dots-9", "dots-9", "dots-9"})).kind == "nineGates", "nine gates");
  require(mahjong::isFlowerWinningHand(tiles({"flower-plum", "flower-orchid", "flower-chrysanthemum", "flower-bamboo"})), "flower win");
}

void testLegalClaims() {
  auto claimTiles = tiles({"dots-3", "dots-1", "dots-2", "dots-2", "dots-4", "dots-4", "dots-5", "dots-3", "dots-3", "dots-3"});
  const auto discard = claimTiles.front();
  claimTiles.erase(claimTiles.begin());
  auto state = roundState();
  state.phase = mahjong::Phase::AwaitingClaims;
  state.players = {emptyPlayer(0), emptyPlayer(1, claimTiles), emptyPlayer(2), emptyPlayer(3)};
  state.players[0].discards = {discard};
  state.lastDiscard = mahjong::LastDiscard{discard, 0, 1};
  const auto actions = mahjong::getLegalActions(state, 1);
  require(std::count_if(actions.begin(), actions.end(), [](const auto& action) { return action.type == mahjong::ActionType::Chow; }) == 3, "three chow actions");
  require(std::any_of(actions.begin(), actions.end(), [](const auto& action) { return action.type == mahjong::ActionType::Pong; }), "pong action");
  require(std::any_of(actions.begin(), actions.end(), [](const auto& action) { return action.type == mahjong::ActionType::Kong && action.kongType == mahjong::KongType::Exposed; }), "exposed kong action");
}

void testSettlement() {
  auto hand = tiles({"dots-1", "dots-1", "dots-1", "dots-2", "dots-2", "dots-2", "dots-3", "dots-3", "dots-3", "dots-4", "dots-4", "dots-4", "east", "east"});
  auto state = roundState();
  state.currentTurn = 1;
  state.players = {emptyPlayer(0), emptyPlayer(1, hand), emptyPlayer(2), emptyPlayer(3)};
  state.lastDraw = mahjong::LastDraw{hand.back(), 1, "liveWall", 1};
  const auto next = mahjong::declareSelfDrawWin(state);
  require(next.phase == mahjong::Phase::Finished, "round finished");
  require(next.conclusion && next.conclusion->settlement, "settlement exists");
  require(next.conclusion->settlement->fan == 7, "expected fan total");
  require(next.conclusion->settlement->basePoints == 48, "new 7 Fan base points");
  require(next.players[1].score == 144, "self-draw winner receives one base payment from each player");
}

void testPaymentTable() {
  const std::vector<int> fanValues{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 99};
  const std::vector<int> expectedPoints{1, 2, 4, 8, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 384};
  std::vector<int> actualPoints;
  for (const auto fan : fanValues) {
    actualPoints.push_back(mahjong::lookupBasePayment(fan));
  }
  require(actualPoints == expectedPoints, "fan to points table matches requested values");
}

void testPaymentTransfers() {
  const auto selfDraw = mahjong::calculatePayments(7, mahjong::Wind::South, "self-pick");
  require(selfDraw.lines.size() == 3, "self-draw has three payer lines");
  require(selfDraw.deltas.at(mahjong::Wind::South) == 144, "self-draw winner receives three base payments");
  require(selfDraw.deltas.at(mahjong::Wind::East) == -48, "self-draw east pays one base payment");
  require(selfDraw.deltas.at(mahjong::Wind::West) == -48, "self-draw west pays one base payment");
  require(selfDraw.deltas.at(mahjong::Wind::North) == -48, "self-draw north pays one base payment");

  const auto discard = mahjong::calculatePayments(7, mahjong::Wind::South, "discard", mahjong::Wind::West);
  require(discard.lines.size() == 1, "discard win has one payer line");
  require(discard.lines.front().from == mahjong::Wind::West && discard.lines.front().points == 48, "discarder pays one base payment");
  require(discard.deltas.at(mahjong::Wind::South) == 48 && discard.deltas.at(mahjong::Wind::West) == -48, "discard deltas match one payer");

  const auto robKong = mahjong::calculatePayments(7, mahjong::Wind::South, "rob-kong", mahjong::Wind::West);
  require(robKong.lines.size() == 1, "robbed Kong win has one liable payer");
  require(robKong.lines.front().from == mahjong::Wind::West && robKong.lines.front().points == 144, "robbed Kong player pays all three payments");
  require(robKong.deltas.at(mahjong::Wind::South) == 144 && robKong.deltas.at(mahjong::Wind::West) == -144, "robbed Kong deltas match liable payer");

  const auto allCalled = mahjong::calculatePayments(7, mahjong::Wind::South, "self-pick-all-called", mahjong::Wind::North);
  require(allCalled.lines.size() == 1, "all-called self-draw has one liable payer");
  require(allCalled.lines.front().from == mahjong::Wind::North && allCalled.lines.front().points == 144, "final meld provider pays all three payments");
  require(allCalled.deltas.at(mahjong::Wind::South) == 144 && allCalled.deltas.at(mahjong::Wind::North) == -144, "all-called deltas match liable payer");
}

void testRobbingKongSettlementResponsibility() {
  auto hand = tiles({"dots-1", "dots-1", "dots-1", "dots-2", "dots-2", "dots-2", "dots-3", "dots-3", "dots-3", "dots-4", "dots-4", "dots-4", "east", "east"});
  const auto winningTile = hand.back();
  hand.pop_back();

  auto state = roundState();
  state.players = {emptyPlayer(0), emptyPlayer(1, hand), emptyPlayer(2), emptyPlayer(3)};

  mahjong::RoundConclusion conclusion;
  conclusion.reason = mahjong::ConclusionReason::Win;
  conclusion.winnerSeat = 1;
  conclusion.winningTile = winningTile;
  conclusion.source = mahjong::WinSource::RobbingKong;
  conclusion.message = "Seat 1 wins by robbing a Kong.";
  conclusion.responsibleSeat = 2;

  const auto settlement = mahjong::scoreWinningRound(state, conclusion);
  require(settlement.fan == 7 && settlement.basePoints == 48, "robbing Kong fixture has 7 Fan base settlement");
  require(settlement.paymentLines.size() == 1, "robbing Kong settlement has one liable payer");
  require(settlement.paymentLines.front().from == mahjong::Wind::West && settlement.paymentLines.front().to == mahjong::Wind::South, "robbed Kong player pays winner");
  require(settlement.paymentLines.front().points == 144, "robbed Kong player pays all players' points");
  require(settlement.deltas.at(mahjong::Wind::South) == 144 && settlement.deltas.at(mahjong::Wind::West) == -144, "robbing Kong settlement deltas match liable payer");
}

void testAllCalledSelfDrawResponsibility() {
  auto pair = tiles({"east", "east"});
  auto winner = emptyPlayer(0, pair);
  winner.melds = {
    calledMeld(mahjong::MeldKind::Pong, {"dots-1", "dots-1", "dots-1"}, 1),
    calledMeld(mahjong::MeldKind::Pong, {"dots-2", "dots-2", "dots-2"}, 2),
    calledMeld(mahjong::MeldKind::Pong, {"dots-3", "dots-3", "dots-3"}, 3),
    calledMeld(mahjong::MeldKind::Pong, {"dots-4", "dots-4", "dots-4"}, 2),
  };

  auto state = roundState();
  state.currentTurn = 0;
  state.players = {winner, emptyPlayer(1), emptyPlayer(2), emptyPlayer(3)};
  state.lastDraw = mahjong::LastDraw{pair.back(), 0, "liveWall", 1};

  const auto next = mahjong::declareSelfDrawWin(state);
  require(next.phase == mahjong::Phase::Finished, "all-called self-draw finishes round");
  require(next.conclusion && next.conclusion->settlement, "all-called settlement exists");
  const auto& settlement = *next.conclusion->settlement;
  require(settlement.fan == 7, "all-called fixture has 7 Fan");
  require(settlement.paymentLines.size() == 1, "all-called self-draw uses one liable payer line");
  require(settlement.paymentLines.front().from == mahjong::Wind::West && settlement.paymentLines.front().points == 144, "last meld provider pays all players' points");
  require(next.players[0].score == 144 && next.players[2].score == -144, "all-called score transfer applies to responsible player");
  require(next.players[1].score == 0 && next.players[3].score == 0, "non-responsible players do not pay all-called self-draw");
}

void testMinimumFanWinLegality() {
  auto lowFanHand = tiles({"dots-1", "dots-2", "dots-3", "dots-4", "dots-5", "dots-6", "bamboo-1", "bamboo-2", "bamboo-3", "characters-1", "characters-2", "characters-3", "east", "east"});

  auto threeFanState = roundState();
  threeFanState.rules.minFan = 3;
  threeFanState.players = {emptyPlayer(0, lowFanHand), emptyPlayer(1), emptyPlayer(2), emptyPlayer(3)};
  threeFanState.lastDraw = mahjong::LastDraw{lowFanHand.back(), 0, "liveWall", 1};
  const auto threeFanActions = mahjong::getLegalActions(threeFanState, 0);
  require(std::none_of(threeFanActions.begin(), threeFanActions.end(), [](const auto& action) {
    return action.type == mahjong::ActionType::Win;
  }), "2 Fan hand cannot win with 3 Fan minimum");

  auto twoFanState = threeFanState;
  twoFanState.rules.minFan = 2;
  const auto twoFanActions = mahjong::getLegalActions(twoFanState, 0);
  require(std::any_of(twoFanActions.begin(), twoFanActions.end(), [](const auto& action) {
    return action.type == mahjong::ActionType::Win;
  }), "2 Fan hand can win when minimum Fan is adjusted to 2");
  const auto twoFanWin = mahjong::declareSelfDrawWin(twoFanState);
  require(twoFanWin.conclusion && twoFanWin.conclusion->settlement, "2 Fan adjusted-minimum settlement exists");
  require(twoFanWin.conclusion->settlement->basePoints == 4 && twoFanWin.players[0].score == 12, "2 Fan adjusted-minimum settlement pays one base payment from each player");
}

void testAiAndRoom() {
  const auto sim = mahjong::runAiRoundSimulation("ai-test", mahjong::AiDifficulty::Medium, 1000);
  require(sim.completed, sim.blocker.empty() ? "ai sim completed" : sim.blocker);

  mahjong::server::RoomManager manager;
  auto created = manager.createRoom("room-test");
  require(created.claimLinks.size() == 4, "room creates claim links");
  auto claimed = manager.claimSeat(created.room.roomCode, 0, created.claimLinks[0].token, "Alice");
  auto snapshot = manager.createSnapshot(claimed.room, 0);
  require(snapshot.players[0].concealedTiles.has_value(), "viewer hand visible");
  require(!snapshot.players[1].concealedTiles.has_value(), "other hand hidden");
  const auto stale = manager.submitHumanAction(claimed.room.roomCode, 0, claimed.sessionToken, 1, snapshot.legalActions.empty() ? mahjong::LegalAction{mahjong::ActionType::Pass} : snapshot.legalActions.front());
  require(!stale.ok && stale.code == "stale_version", "stale commands rejected");
}

void testRoomEviction() {
  using std::chrono::seconds;
  using std::chrono::steady_clock;

  mahjong::server::RoomManager manager;
  auto created = manager.createRoom("evict-test");
  const auto code = created.room.roomCode;
  require(manager.roomCount() == 1, "single room created");

  const auto t0 = steady_clock::now();

  // TTL not yet exceeded → no eviction.
  auto evicted = manager.evictIdleRooms(t0, seconds{60}, seconds{60}, 0);
  require(evicted.empty(), "no rooms evicted before TTL");
  require(manager.roomCount() == 1, "room remains before TTL");

  // Active room (round in progress). ttlActive=10s, now jumped 1h forward → evict.
  evicted = manager.evictIdleRooms(t0 + std::chrono::hours{1}, seconds{60}, seconds{10}, 0);
  require(evicted.size() == 1 && evicted[0] == code, "active room evicted past ttlActive");
  require(manager.roomCount() == 0, "rooms_ is empty after TTL eviction");

  // Touch keeps it alive: create, touch at +1h, evict at +1h+5s with 30s TTL → stays.
  auto fresh = manager.createRoom("touch-test");
  const auto code2 = fresh.room.roomCode;
  manager.touchRoom(code2);  // refreshes to "now"
  const auto t1 = steady_clock::now();
  evicted = manager.evictIdleRooms(t1 + seconds{5}, seconds{60}, seconds{30}, 0);
  require(evicted.empty() && manager.roomCount() == 1, "touched room survives short check");

  // Even with TTLs of 60s, a small maxRooms cap should evict the oldest.
  manager.createRoom("cap-test-a");
  manager.createRoom("cap-test-b");
  require(manager.roomCount() == 3, "three rooms staged for cap test");
  evicted = manager.evictIdleRooms(steady_clock::now(), seconds{3600}, seconds{3600}, 2);
  require(evicted.size() == 1, "cap eviction removes one room");
  require(manager.roomCount() == 2, "room count clamped to cap");
}

void testClaimPriorityOrdering() {
  // Win > exposed Kong > Pong > Chow > anything else.
  mahjong::LegalAction win{};       win.type = mahjong::ActionType::Win;
  mahjong::LegalAction kongExp{};   kongExp.type = mahjong::ActionType::Kong; kongExp.kongType = mahjong::KongType::Exposed;
  mahjong::LegalAction kongCon{};   kongCon.type = mahjong::ActionType::Kong; kongCon.kongType = mahjong::KongType::Concealed;
  mahjong::LegalAction pong{};      pong.type = mahjong::ActionType::Pong;
  mahjong::LegalAction chow{};      chow.type = mahjong::ActionType::Chow;
  mahjong::LegalAction pass{};      pass.type = mahjong::ActionType::Pass;
  mahjong::LegalAction draw{};      draw.type = mahjong::ActionType::Draw;
  require(mahjong::claimPriority(win) == 4, "Win priority is 4");
  require(mahjong::claimPriority(kongExp) == 3, "Exposed Kong priority is 3");
  require(mahjong::claimPriority(pong) == 2, "Pong priority is 2");
  require(mahjong::claimPriority(chow) == 1, "Chow priority is 1");
  require(mahjong::claimPriority(pass) == 0, "Pass priority is 0");
  require(mahjong::claimPriority(draw) == 0, "Draw priority is 0");
  // Concealed/added Kongs are not discard-claims; should not outrank Pong.
  require(mahjong::claimPriority(kongCon) == 0, "Concealed Kong is not a discard claim");
  require(mahjong::claimPriority(win) > mahjong::claimPriority(kongExp), "Win > Kong");
  require(mahjong::claimPriority(kongExp) > mahjong::claimPriority(pong), "Kong > Pong");
  require(mahjong::claimPriority(pong) > mahjong::claimPriority(chow), "Pong > Chow");
}

void testClaimPrecedenceGate() {
  // Build a contrived claim window where seat 1 can Chow a 3-dot (with 2-4)
  // and seat 2 can Pong it (with two 3-dots). Then drive both submissions
  // through RoomManager and verify the Chow is rejected with claim_pending
  // until seat 2 either claims or passes.
  mahjong::server::RoomManager manager;
  // Use a deterministic seed so the deal is reproducible.
  auto created = manager.createRoom("precedence-test-seed");
  const auto code = created.room.roomCode;

  // Claim seats 1 and 2 (humans) so we get session tokens.
  auto claim1 = manager.claimSeat(code, 1, created.claimLinks[1].token, "Bob");
  auto claim2 = manager.claimSeat(code, 2, created.claimLinks[2].token, "Carol");

  // Surgically install a synthetic claim scenario into the room's round
  // state. RoomManager's claim-precedence gate consults `getLegalActions`
  // on each seat, so we just need a state where seat 1 has Chow and seat
  // 2 has Pong on the same discard.
  auto room = *manager.getRoom(code);
  auto discardTile = tiles({"dots-3"}).front();
  // Hand chosen so seat 1 has Chow (dots-2 + dots-4) but no Win or Pong
  // available on dots-3. The rest is a scattering of singles to avoid
  // accidentally completing a winning hand.
  auto seat1Hand = tiles({"dots-2", "dots-4",
                           "characters-1", "characters-2", "characters-5", "characters-7", "characters-9",
                           "bamboo-5", "bamboo-6", "bamboo-7", "bamboo-8", "bamboo-9",
                           "red"});
  // Hand chosen so seat 2 has Pong (two dots-3) but no Win available.
  auto seat2Hand = tiles({"dots-3", "dots-3",
                           "characters-3", "characters-4", "characters-6", "characters-8",
                           "bamboo-1", "bamboo-2", "bamboo-3", "bamboo-4",
                           "green", "white", "north"});
  room.roundState = roundState();
  room.roundState.phase = mahjong::Phase::AwaitingClaims;
  room.roundState.players = {
    emptyPlayer(0),
    emptyPlayer(1, seat1Hand),
    emptyPlayer(2, seat2Hand),
    emptyPlayer(3),
  };
  room.roundState.players[0].discards = {discardTile};
  room.roundState.lastDiscard = mahjong::LastDiscard{discardTile, 0, 1};
  room.pendingClaimPasses.clear();
  manager.injectRoomForTest(room);

  // Pre-checks: seat 1 should have Chow, seat 2 should have Pong.
  const auto seat1Actions = mahjong::getLegalActions(room.roundState, 1);
  const auto seat2Actions = mahjong::getLegalActions(room.roundState, 2);
  auto findChow = [](const std::vector<mahjong::LegalAction>& as) {
    for (const auto& a : as) if (a.type == mahjong::ActionType::Chow) return a;
    throw std::runtime_error("no chow action");
  };
  auto findPong = [](const std::vector<mahjong::LegalAction>& as) {
    for (const auto& a : as) if (a.type == mahjong::ActionType::Pong) return a;
    throw std::runtime_error("no pong action");
  };
  const auto chowAction = findChow(seat1Actions);
  const auto pongAction = findPong(seat2Actions);

  // Seat 1 tries to Chow while seat 2 still has Pong available → REJECTED.
  const auto refreshed = *manager.getRoom(code);
  auto rejected = manager.submitHumanAction(code, 1, claim1.sessionToken, refreshed.version, chowAction);
  require(!rejected.ok && rejected.code == "claim_pending",
          std::string("Chow rejected as claim_pending, got: ") + rejected.code + " — " + rejected.message);

  // Seat 2 plays its Pong → ACCEPTED (no higher claim available).
  auto pongResult = manager.submitHumanAction(code, 2, claim2.sessionToken, refreshed.version, pongAction);
  require(pongResult.ok, std::string("Pong accepted: ") + pongResult.message);

  // Seat 2 must now Discard (Pong applied). Confirm seat 1 can no longer Chow
  // on the (now-consumed) discard tile.
  const auto seat1ActionsAfter = mahjong::getLegalActions(pongResult.room->roundState, 1);
  require(std::none_of(seat1ActionsAfter.begin(), seat1ActionsAfter.end(),
                       [](const mahjong::LegalAction& a) { return a.type == mahjong::ActionType::Chow; }),
          "Seat 1 no longer has Chow after Pong consumed the discard");
}

void testChickenHandSetting() {
  mahjong::server::RoomManager manager;
  auto created = manager.createRoom("chicken-test");
  const auto code = created.room.roomCode;
  require(manager.getMinFan(code) == 3, "Default minFan is 3");
  auto snap = manager.createSnapshot(created.room, std::nullopt);
  require(snap.minFan == 3, "Snapshot exposes default minFan=3");
  require(manager.setMinFan(code, 0), "setMinFan(0) succeeds");
  require(manager.getMinFan(code) == 0, "minFan reads back as 0");
  auto fresh = *manager.getRoom(code);
  require(fresh.roundState.rules.minFan == 0, "Live round rules updated to minFan=0");
  auto snap2 = manager.createSnapshot(fresh, std::nullopt);
  require(snap2.minFan == 0, "Snapshot reflects Chicken Hand minFan=0");
  // Clamping: negative values are floored to 0, values >13 capped at 13.
  manager.setMinFan(code, -5);
  require(manager.getMinFan(code) == 0, "Negative minFan clamped to 0");
  manager.setMinFan(code, 99);
  require(manager.getMinFan(code) == 13, "Excessive minFan clamped to 13");
  require(!manager.setMinFan("NOSUCH", 5), "Unknown room returns false");
}

} // namespace

int main() {
  try {
    testTilesAndWall();
    testHandValidation();
    testLegalClaims();
    testSettlement();
    testPaymentTable();
    testPaymentTransfers();
    testRobbingKongSettlementResponsibility();
    testAllCalledSelfDrawResponsibility();
    testMinimumFanWinLegality();
    testAiAndRoom();
    testRoomEviction();
    testClaimPriorityOrdering();
    testClaimPrecedenceGate();
    testChickenHandSetting();
    std::cout << "All Mahjong C++ tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "Test failure: " << error.what() << "\n";
    return EXIT_FAILURE;
  }
}
