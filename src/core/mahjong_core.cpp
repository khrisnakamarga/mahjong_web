#include "core/mahjong_core.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>

namespace mahjong {
namespace {

constexpr std::array<Wind, 4> kWinds{Wind::East, Wind::South, Wind::West, Wind::North};
constexpr std::array<Suit, 3> kSuits{Suit::Dots, Suit::Bamboo, Suit::Characters};
constexpr std::array<Dragon, 3> kDragons{Dragon::Red, Dragon::Green, Dragon::White};

std::string title(std::string value) {
  if (!value.empty()) {
    value[0] = static_cast<char>(std::toupper(value[0]));
  }
  return value;
}

std::string toString(Suit suit) {
  switch (suit) {
    case Suit::Dots: return "dots";
    case Suit::Bamboo: return "bamboo";
    case Suit::Characters: return "characters";
  }
  throw std::runtime_error("Invalid suit");
}

std::string toString(Dragon dragon) {
  switch (dragon) {
    case Dragon::Red: return "red";
    case Dragon::Green: return "green";
    case Dragon::White: return "white";
  }
  throw std::runtime_error("Invalid dragon");
}

uint32_t hashSeed(const std::string& seed) {
  uint32_t hash = 2166136261u;
  for (const unsigned char c : seed) {
    hash ^= c;
    hash *= 16777619u;
  }
  return hash;
}

class SeedRandom {
 public:
  explicit SeedRandom(const std::string& seed) : value_(hashSeed(seed) ? hashSeed(seed) : 0x6d2b79f5u) {}

  double next() {
    value_ += 0x6d2b79f5u;
    uint32_t next = value_;
    next = (next ^ (next >> 15)) * (next | 1u);
    next ^= next + ((next ^ (next >> 7)) * (next | 61u));
    return static_cast<double>((next ^ (next >> 14))) / 4294967296.0;
  }

 private:
  uint32_t value_;
};

Tile makeTile(std::string key, Category category, std::string name, int copy) {
  Tile tile;
  tile.key = std::move(key);
  tile.category = category;
  tile.name = std::move(name);
  tile.id = tile.key + "#" + std::to_string(copy);
  tile.copy = copy;
  if (category == Category::Suit) {
    const auto dash = tile.key.find('-');
    const auto suit = tile.key.substr(0, dash);
    tile.suit = suit == "dots" ? Suit::Dots : suit == "bamboo" ? Suit::Bamboo : Suit::Characters;
    tile.rank = std::stoi(tile.key.substr(dash + 1));
  } else if (category == Category::Wind) {
    if (tile.key == "east") tile.wind = Wind::East;
    if (tile.key == "south") tile.wind = Wind::South;
    if (tile.key == "west") tile.wind = Wind::West;
    if (tile.key == "north") tile.wind = Wind::North;
  } else if (category == Category::Dragon) {
    if (tile.key == "red") tile.dragon = Dragon::Red;
    if (tile.key == "green") tile.dragon = Dragon::Green;
    if (tile.key == "white") tile.dragon = Dragon::White;
  }
  return tile;
}

Tile tileFromKey(const std::string& key, std::set<std::string>& used) {
  const auto set = createTileSet();
  for (const auto& tile : set) {
    if (tile.key == key && !used.contains(tile.id)) {
      used.insert(tile.id);
      return tile;
    }
  }
  throw std::runtime_error("No unused tile for " + key);
}

std::map<std::string, int> countTileKeys(const std::vector<Tile>& tiles) {
  std::map<std::string, int> counts;
  for (const auto& tile : tiles) {
    if (!isFlowerOrSeason(tile)) {
      counts[tile.key] += 1;
    }
  }
  return counts;
}

std::optional<std::string> firstRemainingKey(const std::map<std::string, int>& counts) {
  for (const auto& [key, count] : counts) {
    if (count > 0) return key;
  }
  return std::nullopt;
}

void decrement(std::map<std::string, int>& counts, const std::string& key, int amount) {
  const auto current = counts[key];
  if (current < amount) {
    throw std::runtime_error("Cannot remove tile count");
  }
  if (current == amount) {
    counts.erase(key);
  } else {
    counts[key] = current - amount;
  }
}

std::optional<std::string> nextRankKey(const std::string& key, int offset) {
  const auto dash = key.find('-');
  if (dash == std::string::npos) return std::nullopt;
  const auto suit = key.substr(0, dash);
  const int rank = std::stoi(key.substr(dash + 1));
  if (rank + offset > 9) return std::nullopt;
  return suit + "-" + std::to_string(rank + offset);
}

bool canFormSets(std::map<std::string, int> counts, int setsNeeded) {
  if (setsNeeded == 0) return !firstRemainingKey(counts).has_value();
  const auto key = firstRemainingKey(counts);
  if (!key) return false;

  if (counts[*key] >= 3) {
    auto pongCounts = counts;
    decrement(pongCounts, *key, 3);
    if (canFormSets(pongCounts, setsNeeded - 1)) return true;
  }

  const auto second = nextRankKey(*key, 1);
  const auto third = nextRankKey(*key, 2);
  if (second && third && counts[*second] > 0 && counts[*third] > 0) {
    auto chowCounts = counts;
    decrement(chowCounts, *key, 1);
    decrement(chowCounts, *second, 1);
    decrement(chowCounts, *third, 1);
    if (canFormSets(chowCounts, setsNeeded - 1)) return true;
  }

  return false;
}

PlayerState getPlayer(const RoundState& state, int seatIndex) {
  if (seatIndex < 0 || seatIndex >= static_cast<int>(state.players.size())) {
    throw std::runtime_error("Invalid seat index");
  }
  return state.players[static_cast<std::size_t>(seatIndex)];
}

Wind windForSeat(int seatIndex, int dealerSeat) {
  return kWinds[static_cast<std::size_t>((seatIndex - dealerSeat + 4) % 4)];
}

int nextSeatIndex(int seatIndex) {
  return (seatIndex + 1) % 4;
}

Wind nextWind(Wind wind) {
  const auto it = std::find(kWinds.begin(), kWinds.end(), wind);
  const auto index = static_cast<int>(std::distance(kWinds.begin(), it));
  return kWinds[static_cast<std::size_t>((index + 1) % 4)];
}

std::vector<Tile> removeTileIds(const std::vector<Tile>& tiles, const std::vector<std::string>& ids) {
  auto remaining = ids;
  std::vector<Tile> result;
  for (const auto& tile : tiles) {
    const auto it = std::find(remaining.begin(), remaining.end(), tile.id);
    if (it == remaining.end()) {
      result.push_back(tile);
    } else {
      remaining.erase(it);
    }
  }
  if (!remaining.empty()) {
    throw std::runtime_error("Tile not found: " + remaining.front());
  }
  return result;
}

struct DrawResult {
  WallState wall;
  std::optional<Tile> tile;
};

DrawResult drawFromLiveWall(const WallState& wall) {
  if (wall.liveWall.empty()) return {wall, std::nullopt};
  auto next = wall;
  auto tile = next.liveWall.front();
  next.liveWall.erase(next.liveWall.begin());
  return {next, tile};
}

DrawResult drawReplacementTile(const WallState& wall) {
  if (wall.deadWall.empty()) return {wall, std::nullopt};
  auto next = wall;
  auto tile = next.deadWall.back();
  next.deadWall.pop_back();
  next.replacementDraws.push_back(tile);
  return {next, tile};
}

struct PlayerDrawResult {
  WallState wall;
  PlayerState player;
  std::optional<Tile> drawnTile;
};

PlayerDrawResult addDrawnTileReplacingFlowers(PlayerState player, WallState wall, const Tile& tile) {
  if (!isFlowerOrSeason(tile)) {
    player.concealedTiles.push_back(tile);
    return {wall, player, tile};
  }
  player.flowers.push_back(tile);
  auto replacement = drawReplacementTile(wall);
  if (!replacement.tile) return {replacement.wall, player, std::nullopt};
  return addDrawnTileReplacingFlowers(player, replacement.wall, *replacement.tile);
}

PlayerDrawResult drawLiveTileReplacingFlowers(PlayerState player, WallState wall) {
  auto draw = drawFromLiveWall(wall);
  if (!draw.tile) return {draw.wall, player, std::nullopt};
  return addDrawnTileReplacingFlowers(std::move(player), std::move(draw.wall), *draw.tile);
}

PlayerDrawResult drawKongReplacementReplacingFlowers(PlayerState player, WallState wall) {
  auto draw = drawReplacementTile(wall);
  if (!draw.tile) return {draw.wall, player, std::nullopt};
  return addDrawnTileReplacingFlowers(std::move(player), std::move(draw.wall), *draw.tile);
}

std::vector<PlayerState> replacePlayer(std::vector<PlayerState> players, int seatIndex, const PlayerState& player) {
  players[static_cast<std::size_t>(seatIndex)] = player;
  return players;
}

RoundState withDealtTile(const RoundState& state, int seatIndex) {
  auto next = state;
  auto result = drawLiveTileReplacingFlowers(getPlayer(next, seatIndex), next.wall);
  next.wall = result.wall;
  next.players = replacePlayer(next.players, seatIndex, result.player);
  return next;
}

RoundState dealInitialHands(const RoundState& state) {
  auto next = state;
  for (int drawIndex = 0; drawIndex < 13; ++drawIndex) {
    for (int seat = 0; seat < 4; ++seat) {
      next = withDealtTile(next, seat);
    }
  }
  return withDealtTile(next, next.dealerSeat);
}

std::string actionStableKey(const LegalAction& action) {
  std::ostringstream out;
  out << static_cast<int>(action.type) << "|" << action.tileId << "|" << static_cast<int>(action.source)
      << "|" << action.claimedTileId << "|" << static_cast<int>(action.kongType) << "|" << action.tileKey
      << "|" << action.meldId;
  for (const auto& tile : action.tiles) out << "|" << tile;
  return out.str();
}

std::vector<std::string> tupleIds(const std::vector<Tile>& tiles, int count) {
  if (static_cast<int>(tiles.size()) < count) throw std::runtime_error("Expected more tiles");
  std::vector<std::string> ids;
  for (int index = 0; index < count; ++index) ids.push_back(tiles[static_cast<std::size_t>(index)].id);
  return ids;
}

std::vector<Tile> matchingTiles(const std::vector<Tile>& tiles, const std::string& key, int count) {
  std::vector<Tile> result;
  for (const auto& tile : tiles) {
    if (tile.key == key) {
      result.push_back(tile);
      if (static_cast<int>(result.size()) == count) return result;
    }
  }
  throw std::runtime_error("Not enough matching tiles for " + key);
}

bool isStandardWinningHand(const std::vector<Tile>& tiles, const std::vector<Meld>& melds) {
  std::vector<Tile> concealed;
  std::copy_if(tiles.begin(), tiles.end(), std::back_inserter(concealed), [](const Tile& tile) { return !isFlowerOrSeason(tile); });
  const int setsNeeded = 4 - static_cast<int>(melds.size());
  if (setsNeeded < 0 || static_cast<int>(concealed.size()) != setsNeeded * 3 + 2) return false;
  const auto counts = countTileKeys(concealed);
  for (const auto& [pairKey, count] : counts) {
    if (count < 2) continue;
    auto remainder = counts;
    decrement(remainder, pairKey, 2);
    if (canFormSets(remainder, setsNeeded)) return true;
  }
  return false;
}

bool isSevenPairsShape(const std::vector<Tile>& tiles) {
  std::vector<Tile> concealed;
  std::copy_if(tiles.begin(), tiles.end(), std::back_inserter(concealed), [](const Tile& tile) { return !isFlowerOrSeason(tile); });
  if (concealed.size() != 14) return false;
  const auto counts = countTileKeys(concealed);
  int pairs = 0;
  for (const auto& [_, count] : counts) {
    if (count != 2 && count != 4) return false;
    pairs += count / 2;
  }
  return pairs == 7;
}

const std::set<std::string> terminalHonorKeys() {
  return {"dots-1", "dots-9", "bamboo-1", "bamboo-9", "characters-1", "characters-9", "east", "south", "west", "north", "red", "green", "white"};
}

bool isThirteenOrphansShape(const std::vector<Tile>& tiles) {
  std::vector<Tile> concealed;
  std::copy_if(tiles.begin(), tiles.end(), std::back_inserter(concealed), [](const Tile& tile) { return !isFlowerOrSeason(tile); });
  if (concealed.size() != 14) return false;
  const auto required = terminalHonorKeys();
  const auto counts = countTileKeys(concealed);
  bool duplicate = false;
  for (const auto& key : required) {
    const auto count = counts.contains(key) ? counts.at(key) : 0;
    if (count == 0 || count > 2) return false;
    if (count == 2) {
      if (duplicate) return false;
      duplicate = true;
    }
  }
  return duplicate && std::all_of(counts.begin(), counts.end(), [&](const auto& item) { return required.contains(item.first); });
}

bool isNineGatesShape(const std::vector<Tile>& tiles) {
  std::vector<Tile> concealed;
  std::copy_if(tiles.begin(), tiles.end(), std::back_inserter(concealed), [](const Tile& tile) { return !isFlowerOrSeason(tile); });
  if (concealed.size() != 14 || !concealed.front().suit) return false;
  const auto suit = *concealed.front().suit;
  if (!std::all_of(concealed.begin(), concealed.end(), [&](const Tile& tile) { return tile.suit && *tile.suit == suit; })) return false;
  const auto counts = countTileKeys(concealed);
  const std::array<int, 9> required{3, 1, 1, 1, 1, 1, 1, 1, 3};
  for (int rank = 1; rank <= 9; ++rank) {
    const auto key = toString(suit) + "-" + std::to_string(rank);
    const auto count = counts.contains(key) ? counts.at(key) : 0;
    if (count < required[static_cast<std::size_t>(rank - 1)]) return false;
  }
  return true;
}

Meld makeMeld(MeldKind kind, const std::vector<Tile>& tiles, const std::string& claimedTileId = "", std::optional<int> fromSeat = std::nullopt) {
  Meld meld;
  meld.kind = kind;
  std::ostringstream id;
  id << toString(kind == MeldKind::Chow ? ActionType::Chow : kind == MeldKind::Pong ? ActionType::Pong : ActionType::Kong) << "-";
  for (const auto& tile : tiles) id << tile.id << "|";
  meld.id = id.str();
  meld.tiles = tiles;
  if (!claimedTileId.empty()) meld.claimedTileId = claimedTileId;
  meld.fromSeat = fromSeat;
  meld.concealed = kind == MeldKind::ConcealedKong;
  return meld;
}

std::vector<PlayerState> removeLastDiscardFromOwner(const RoundState& state) {
  auto players = state.players;
  if (!state.lastDiscard) return players;
  auto owner = players[static_cast<std::size_t>(state.lastDiscard->bySeat)];
  owner.discards.erase(std::remove_if(owner.discards.begin(), owner.discards.end(), [&](const Tile& tile) {
    return tile.id == state.lastDiscard->tile.id;
  }), owner.discards.end());
  players[static_cast<std::size_t>(state.lastDiscard->bySeat)] = owner;
  return players;
}

bool isPongLike(const Meld& meld) {
  return meld.kind == MeldKind::Pong || meld.kind == MeldKind::ExposedKong || meld.kind == MeldKind::ConcealedKong || meld.kind == MeldKind::AddedKong;
}

std::string primaryKey(const Meld& meld) {
  if (meld.tiles.empty()) return "";
  return meld.tiles.front().key;
}

struct FanOccurrence {
  std::string id;
  std::string name;
  int fan{};
  std::string source;
  std::string replacementGroup;
  std::vector<std::string> replaces;
};

void addFeature(std::vector<FanOccurrence>& features, const std::string& id, const HongKongRules& rules, const std::string& source = "") {
  const auto it = rules.fanTable.find(id);
  if (it == rules.fanTable.end() || !it->second.enabled) return;
  features.push_back({id, it->second.name, it->second.fan, source, it->second.replacementGroup, it->second.replaces});
}

bool hasKeyKind(const std::string& key, Category category) {
  if (category == Category::Wind) return key == "east" || key == "south" || key == "west" || key == "north";
  if (category == Category::Dragon) return key == "red" || key == "green" || key == "white";
  if (category == Category::Suit) return key.find('-') != std::string::npos;
  return false;
}

std::vector<FanOccurrence> resolveIncluded(std::vector<FanOccurrence> features, std::vector<RoundFanFeature>& excluded) {
  std::vector<bool> removed(features.size(), false);
  std::map<std::string, std::vector<std::size_t>> groups;
  for (std::size_t i = 0; i < features.size(); ++i) {
    if (!features[i].replacementGroup.empty()) groups[features[i].replacementGroup].push_back(i);
  }
  for (const auto& [_, indices] : groups) {
    auto best = indices.front();
    for (const auto index : indices) {
      if (features[index].fan > features[best].fan) best = index;
    }
    for (const auto index : indices) {
      if (index != best) {
        removed[index] = true;
        excluded.push_back({features[index].id, features[index].name, features[index].fan, std::nullopt, features[best].id});
      }
    }
  }
  std::vector<std::size_t> order(features.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [&](auto left, auto right) { return features[left].fan > features[right].fan; });
  for (const auto replacementIndex : order) {
    if (removed[replacementIndex]) continue;
    for (const auto& replacedId : features[replacementIndex].replaces) {
      for (std::size_t i = 0; i < features.size(); ++i) {
        if (i != replacementIndex && !removed[i] && features[i].id == replacedId) {
          removed[i] = true;
          excluded.push_back({features[i].id, features[i].name, features[i].fan, std::nullopt, features[replacementIndex].id});
        }
      }
    }
  }
  std::vector<FanOccurrence> included;
  for (std::size_t i = 0; i < features.size(); ++i) {
    if (!removed[i]) included.push_back(features[i]);
  }
  return included;
}

std::vector<Meld> decomposeStandardConcealedMelds(const std::vector<Tile>& tiles, int exposedMeldCount) {
  const int setsNeeded = 4 - exposedMeldCount;
  std::map<std::string, std::vector<Tile>> groups;
  for (const auto& tile : sortTiles(tiles)) {
    if (!isFlowerOrSeason(tile)) groups[tile.key].push_back(tile);
  }
  if (setsNeeded < 0 || static_cast<int>(tiles.size()) != setsNeeded * 3 + 2) return {};

  std::function<std::optional<std::vector<Meld>>(std::map<std::string, std::vector<Tile>>, int)> decompose =
    [&](std::map<std::string, std::vector<Tile>> current, int needed) -> std::optional<std::vector<Meld>> {
      if (needed == 0) {
        return std::all_of(current.begin(), current.end(), [](const auto& item) { return item.second.empty(); })
          ? std::optional<std::vector<Meld>>{std::vector<Meld>{}}
          : std::nullopt;
      }
      auto it = std::find_if(current.begin(), current.end(), [](const auto& item) { return !item.second.empty(); });
      if (it == current.end()) return std::nullopt;
      const auto key = it->first;
      if (it->second.size() >= 3) {
        auto next = current;
        std::vector<Tile> meldTiles(next[key].begin(), next[key].begin() + 3);
        next[key].erase(next[key].begin(), next[key].begin() + 3);
        auto remainder = decompose(next, needed - 1);
        if (remainder) {
          remainder->insert(remainder->begin(), makeMeld(MeldKind::Pong, meldTiles));
          return remainder;
        }
      }
      const auto second = nextRankKey(key, 1);
      const auto third = nextRankKey(key, 2);
      if (second && third && !current[*second].empty() && !current[*third].empty()) {
        auto next = current;
        std::vector<Tile> meldTiles{next[key].front(), next[*second].front(), next[*third].front()};
        next[key].erase(next[key].begin());
        next[*second].erase(next[*second].begin());
        next[*third].erase(next[*third].begin());
        auto remainder = decompose(next, needed - 1);
        if (remainder) {
          remainder->insert(remainder->begin(), makeMeld(MeldKind::Chow, meldTiles));
          return remainder;
        }
      }
      return std::nullopt;
    };

  for (const auto& [key, candidates] : groups) {
    if (candidates.size() < 2) continue;
    auto next = groups;
    next[key].erase(next[key].begin(), next[key].begin() + 2);
    auto result = decompose(next, setsNeeded);
    if (result) return *result;
  }
  return {};
}

RoundState finish(const RoundState& state, RoundConclusion conclusion) {
  auto next = state;
  next.phase = Phase::Finished;
  next.lastDiscard.reset();
  next.lastDraw.reset();
  if (conclusion.reason == ConclusionReason::Win) {
    conclusion.settlement = scoreWinningRound(state, conclusion);
    for (auto& player : next.players) {
      player.score += conclusion.settlement->deltas[player.wind];
    }
  }
  next.conclusion = conclusion;
  return next;
}

bool candidateWinMeetsMinimumFan(const RoundState& state, int winnerSeat, WinSource source, std::optional<Tile> winningTile) {
  RoundConclusion conclusion;
  conclusion.reason = ConclusionReason::Win;
  conclusion.winnerSeat = winnerSeat;
  conclusion.source = source;
  conclusion.winningTile = winningTile;
  try {
    const auto settlement = scoreWinningRound(state, conclusion);
    return settlement.fan >= state.rules.minFan;
  } catch (const std::exception&) {
    return false;
  }
}

int claimSeatDistance(const RoundState& state, int seatIndex) {
  const int bySeat = state.lastDiscard ? state.lastDiscard->bySeat : state.currentTurn;
  int distance = 1;
  int cursor = nextSeatIndex(bySeat);
  while (cursor != seatIndex && distance < 4) {
    cursor = nextSeatIndex(cursor);
    ++distance;
  }
  return distance;
}

RoundState applyLegalAction(const RoundState& state, int seatIndex, const LegalAction& action) {
  if (action.type == ActionType::NextRound) return createNextRoundState(state);
  if (action.type == ActionType::Draw) return drawTile(state, seatIndex);
  if (action.type == ActionType::Discard) return discardTile(state, action.tileId);
  if (action.type == ActionType::Pass) return state.phase == Phase::AwaitingClaims ? passClaimWindow(state) : state;
  if (action.type == ActionType::Win) return state.phase == Phase::AwaitingClaims ? claimDiscard(state, seatIndex, action) : declareSelfDrawWin(state);
  if (action.type == ActionType::Kong && action.kongType != KongType::Exposed) return declareKong(state, action.tileKey, action.meldId);
  return claimDiscard(state, seatIndex, action);
}

int suitedNeighborCount(const Tile& tile, const std::vector<Tile>& tiles) {
  if (!tile.suit || !tile.rank) return 0;
  int score = 0;
  for (const auto& other : tiles) {
    if (!other.suit || !other.rank || other.id == tile.id || *other.suit != *tile.suit) continue;
    const int distance = std::abs(*other.rank - *tile.rank);
    if (distance == 1) score += 2;
    if (distance == 2) score += 1;
  }
  return score;
}

int tileRetentionScore(const Tile& tile, const std::vector<Tile>& tiles, const RoundState& state, int seatIndex) {
  const auto counts = countTileKeys(tiles);
  const int matching = counts.contains(tile.key) ? counts.at(tile.key) : 0;
  int score = matching >= 3 ? 9 : matching == 2 ? 6 : 0;
  score += suitedNeighborCount(tile, tiles);
  if (isHonorTile(tile)) {
    score += matching >= 2 ? 3 : -2;
    if (tile.key == toString(getPlayer(state, seatIndex).wind) || tile.key == toString(state.prevailingWind)) score += 2;
  }
  if (tile.rank && (*tile.rank == 1 || *tile.rank == 9)) score -= 1;
  return score;
}

int handEfficiencyScore(const std::vector<Tile>& tiles, const RoundState& state, int seatIndex) {
  const auto counts = countTileKeys(tiles);
  int score = 0;
  for (const auto& [key, count] : counts) {
    score += count >= 3 ? 12 : count == 2 ? 5 : 0;
    if ((key == toString(getPlayer(state, seatIndex).wind) || key == toString(state.prevailingWind)) && hasKeyKind(key, Category::Wind)) {
      score += count * 2;
    }
  }
  for (const auto& tile : tiles) score += suitedNeighborCount(tile, tiles);
  return score;
}

int discardScore(const LegalAction& action, const RoundState& state, int seatIndex, AiDifficulty difficulty) {
  if (action.type != ActionType::Discard) return std::numeric_limits<int>::min();
  const auto player = getPlayer(state, seatIndex);
  const auto it = std::find_if(player.concealedTiles.begin(), player.concealedTiles.end(), [&](const Tile& tile) { return tile.id == action.tileId; });
  if (it == player.concealedTiles.end()) return std::numeric_limits<int>::min();
  auto remaining = player.concealedTiles;
  remaining.erase(std::remove_if(remaining.begin(), remaining.end(), [&](const Tile& tile) { return tile.id == action.tileId; }), remaining.end());
  int score = handEfficiencyScore(remaining, state, seatIndex) - tileRetentionScore(*it, player.concealedTiles, state, seatIndex);
  if (difficulty == AiDifficulty::Hard) score -= tileKeySortValue(it->key) / 1000;
  return score;
}

int claimScore(const LegalAction& action, const RoundState& state, AiDifficulty difficulty) {
  if (action.type == ActionType::Win) return 10000;
  if (action.type == ActionType::Pass) return difficulty == AiDifficulty::Hard ? 25 : 0;
  if (action.type == ActionType::Draw) return 9000;
  if (action.type == ActionType::Kong) return action.kongType == KongType::Exposed ? 700 : 800;
  if (action.type == ActionType::Pong) {
    const auto valueBonus = state.lastDiscard && isHonorTile(state.lastDiscard->tile) ? 80 : 0;
    return difficulty == AiDifficulty::Hard ? 420 + valueBonus : 500;
  }
  if (action.type == ActionType::Chow) {
    if (difficulty == AiDifficulty::Hard && state.lastDiscard && state.lastDiscard->tile.rank) {
      const auto rank = *state.lastDiscard->tile.rank;
      return rank >= 3 && rank <= 7 ? 260 : 120;
    }
    return 300;
  }
  return std::numeric_limits<int>::min();
}

} // namespace

int claimPriority(const LegalAction& action) {
  if (action.type == ActionType::Win) return 4;
  if (action.type == ActionType::Kong && action.kongType == KongType::Exposed) return 3;
  if (action.type == ActionType::Pong) return 2;
  if (action.type == ActionType::Chow) return 1;
  return 0;
}

std::string toString(Wind wind) {
  switch (wind) {
    case Wind::East: return "east";
    case Wind::South: return "south";
    case Wind::West: return "west";
    case Wind::North: return "north";
  }
  throw std::runtime_error("Invalid wind");
}

std::string toString(Category category) {
  switch (category) {
    case Category::Suit: return "suit";
    case Category::Wind: return "wind";
    case Category::Dragon: return "dragon";
    case Category::Flower: return "flower";
    case Category::Season: return "season";
  }
  throw std::runtime_error("Invalid category");
}

std::string toString(ActionType type) {
  switch (type) {
    case ActionType::NextRound: return "nextRound";
    case ActionType::Draw: return "draw";
    case ActionType::Discard: return "discard";
    case ActionType::Win: return "win";
    case ActionType::Chow: return "chow";
    case ActionType::Pong: return "pong";
    case ActionType::Kong: return "kong";
    case ActionType::Pass: return "pass";
  }
  throw std::runtime_error("Invalid action type");
}

std::string toString(Phase phase) {
  switch (phase) {
    case Phase::AwaitingDraw: return "awaitingDraw";
    case Phase::AwaitingDiscard: return "awaitingDiscard";
    case Phase::AwaitingClaims: return "awaitingClaims";
    case Phase::Finished: return "finished";
  }
  throw std::runtime_error("Invalid phase");
}

std::string tileFace(const Tile& tile) {
  if (tile.category == Category::Suit) {
    const auto mark = tile.suit == Suit::Dots ? "o" : tile.suit == Suit::Bamboo ? "b" : "m";
    return std::to_string(*tile.rank) + mark;
  }
  if (tile.category == Category::Wind) return toString(*tile.wind).substr(0, 1);
  if (tile.category == Category::Dragon) return toString(*tile.dragon).substr(0, 1);
  if (tile.category == Category::Flower) return "F";
  return "S";
}

bool operator==(const LegalAction& left, const LegalAction& right) {
  return left.type == right.type &&
    left.tileId == right.tileId &&
    left.source == right.source &&
    left.claimedTileId == right.claimedTileId &&
    left.tiles == right.tiles &&
    left.kongType == right.kongType &&
    left.tileKey == right.tileKey &&
    left.meldId == right.meldId;
}

std::vector<Tile> createTileSet() {
  std::vector<Tile> tiles;
  for (const auto suit : kSuits) {
    for (int rank = 1; rank <= 9; ++rank) {
      const auto key = toString(suit) + "-" + std::to_string(rank);
      for (int copy = 0; copy < 4; ++copy) tiles.push_back(makeTile(key, Category::Suit, std::to_string(rank) + " " + title(toString(suit)), copy));
    }
  }
  for (const auto wind : kWinds) {
    for (int copy = 0; copy < 4; ++copy) tiles.push_back(makeTile(toString(wind), Category::Wind, title(toString(wind)) + " Wind", copy));
  }
  for (const auto dragon : kDragons) {
    for (int copy = 0; copy < 4; ++copy) tiles.push_back(makeTile(toString(dragon), Category::Dragon, title(toString(dragon)) + " Dragon", copy));
  }
  const std::array<std::string, 4> flowers{"plum", "orchid", "chrysanthemum", "bamboo"};
  const std::array<std::string, 4> seasons{"spring", "summer", "autumn", "winter"};
  for (int index = 0; index < 4; ++index) tiles.push_back(makeTile("flower-" + flowers[static_cast<std::size_t>(index)], Category::Flower, title(flowers[static_cast<std::size_t>(index)]), 0));
  for (int index = 0; index < 4; ++index) tiles.push_back(makeTile("season-" + seasons[static_cast<std::size_t>(index)], Category::Season, title(seasons[static_cast<std::size_t>(index)]), 0));
  return tiles;
}

WallState generateWall(const std::string& seed) {
  auto tiles = createTileSet();
  SeedRandom random(seed);
  for (int index = static_cast<int>(tiles.size()) - 1; index > 0; --index) {
    const int swapIndex = static_cast<int>(std::floor(random.next() * (index + 1)));
    std::swap(tiles[static_cast<std::size_t>(index)], tiles[static_cast<std::size_t>(swapIndex)]);
  }
  WallState wall;
  wall.seed = seed;
  wall.liveWall.assign(tiles.begin(), tiles.end() - 14);
  wall.deadWall.assign(tiles.end() - 14, tiles.end());
  return wall;
}

std::vector<Tile> sortTiles(std::vector<Tile> tiles) {
  std::sort(tiles.begin(), tiles.end(), [](const Tile& left, const Tile& right) {
    const auto sortDelta = tileKeySortValue(left.key) - tileKeySortValue(right.key);
    return sortDelta == 0 ? left.id < right.id : sortDelta < 0;
  });
  return tiles;
}

bool isFlowerOrSeason(const Tile& tile) {
  return tile.category == Category::Flower || tile.category == Category::Season;
}

bool isHonorTile(const Tile& tile) {
  return tile.category == Category::Wind || tile.category == Category::Dragon;
}

bool isSuitTile(const Tile& tile) {
  return tile.category == Category::Suit;
}

int tileKeySortValue(const std::string& key) {
  const auto dash = key.find('-');
  if (dash != std::string::npos && key.rfind("flower-", 0) != 0 && key.rfind("season-", 0) != 0) {
    const auto suit = key.substr(0, dash);
    const int suitIndex = suit == "dots" ? 0 : suit == "bamboo" ? 1 : 2;
    return suitIndex * 10 + std::stoi(key.substr(dash + 1));
  }
  if (key == "east") return 100;
  if (key == "south") return 101;
  if (key == "west") return 102;
  if (key == "north") return 103;
  if (key == "red") return 110;
  if (key == "green") return 111;
  if (key == "white") return 112;
  if (key == "flower-plum") return 120;
  if (key == "flower-orchid") return 121;
  if (key == "flower-chrysanthemum") return 122;
  if (key == "flower-bamboo") return 123;
  if (key == "season-spring") return 130;
  if (key == "season-summer") return 131;
  if (key == "season-autumn") return 132;
  if (key == "season-winter") return 133;
  throw std::runtime_error("Unknown tile key: " + key);
}

RoundState createInitialRoundState(
  const std::string& seed,
  int dealerSeat,
  Wind prevailingWind,
  std::optional<int> windRoundStartDealerSeat,
  RoundRules rules,
  std::vector<Controller> controllers,
  std::vector<std::string> displayNames,
  std::vector<int> scores
) {
  RoundState state;
  state.phase = Phase::AwaitingDiscard;
  state.rules = std::move(rules);
  state.dealerSeat = dealerSeat;
  state.windRoundStartDealerSeat = windRoundStartDealerSeat.value_or(dealerSeat);
  state.prevailingWind = prevailingWind;
  state.currentTurn = dealerSeat;
  state.turnNumber = 1;
  state.wall = generateWall(seed);
  for (int seat = 0; seat < 4; ++seat) {
    PlayerState player;
    player.seatIndex = seat;
    player.wind = windForSeat(seat, dealerSeat);
    player.controller = seat < static_cast<int>(controllers.size()) ? controllers[static_cast<std::size_t>(seat)] : Controller::Ai;
    player.displayName = seat < static_cast<int>(displayNames.size()) ? displayNames[static_cast<std::size_t>(seat)] : "AI " + std::to_string(seat + 1);
    player.score = seat < static_cast<int>(scores.size()) ? scores[static_cast<std::size_t>(seat)] : 0;
    state.players.push_back(player);
  }
  return dealInitialHands(state);
}

RoundState createNextRoundState(const RoundState& state, const std::string& seed) {
  if (state.phase != Phase::Finished) throw std::runtime_error("Cannot start next round before finish");
  const bool dealerRetains = !state.conclusion || state.conclusion->reason != ConclusionReason::Win || !state.conclusion->winnerSeat || *state.conclusion->winnerSeat == state.dealerSeat;
  const int dealerSeat = dealerRetains ? state.dealerSeat : nextSeatIndex(state.dealerSeat);
  const int windRoundStart = state.windRoundStartDealerSeat.value_or(0);
  const bool advancedWind = !dealerRetains && dealerSeat == windRoundStart;
  const Wind prevailingWind = advancedWind ? nextWind(state.prevailingWind) : state.prevailingWind;
  const int nextWindRoundStart = advancedWind ? dealerSeat : windRoundStart;
  std::vector<Controller> controllers;
  std::vector<std::string> names;
  std::vector<int> scores;
  for (const auto& player : state.players) {
    controllers.push_back(player.controller);
    names.push_back(player.displayName);
    scores.push_back(player.score);
  }
  const auto nextSeed = seed.empty()
    ? state.wall.seed + ":next:" + std::to_string(state.turnNumber) + ":" + std::to_string(dealerSeat) + ":" + toString(prevailingWind)
    : seed;
  return createInitialRoundState(nextSeed, dealerSeat, prevailingWind, nextWindRoundStart, state.rules, controllers, names, scores);
}

WinValidationResult validateWinningHand(const std::vector<Tile>& tiles, const std::vector<Meld>& melds, const std::vector<Tile>& flowers, bool allowSpecialHands, bool allowFlowerWins) {
  if (allowFlowerWins && isFlowerWinningHand(flowers)) return {true, "flowerWin", ""};
  if (allowSpecialHands && melds.empty()) {
    if (isSevenPairsShape(tiles)) return {true, "sevenPairs", ""};
    if (isThirteenOrphansShape(tiles)) return {true, "thirteenOrphans", ""};
    if (isNineGatesShape(tiles)) return {true, "nineGates", ""};
  }
  if (isStandardWinningHand(tiles, melds)) return {true, "standard", ""};
  return {false, "", "Hand does not match a supported Hong Kong Mahjong winning shape."};
}

bool isFlowerWinningHand(const std::vector<Tile>& flowers) {
  std::set<std::string> keys;
  for (const auto& tile : flowers) keys.insert(tile.key);
  const bool hasAllFlowers = keys.contains("flower-plum") && keys.contains("flower-orchid") && keys.contains("flower-chrysanthemum") && keys.contains("flower-bamboo");
  const bool hasAllSeasons = keys.contains("season-spring") && keys.contains("season-summer") && keys.contains("season-autumn") && keys.contains("season-winter");
  return flowers.size() >= 8 || hasAllFlowers || hasAllSeasons;
}

std::vector<std::vector<std::string>> getChowWaitTileKeys(const Tile& tile) {
  if (!tile.rank || !tile.suit) return {};
  std::vector<std::vector<std::string>> combinations;
  for (int start : {*tile.rank - 2, *tile.rank - 1, *tile.rank}) {
    if (start < 1 || start + 2 > 9) continue;
    std::vector<std::string> keys;
    for (int rank = start; rank <= start + 2; ++rank) {
      if (rank != *tile.rank) keys.push_back(toString(*tile.suit) + "-" + std::to_string(rank));
    }
    combinations.push_back(keys);
  }
  return combinations;
}

std::vector<LegalAction> getLegalActions(const RoundState& state, int seatIndex) {
  if (state.phase == Phase::Finished) return {{ActionType::NextRound}};
  if (state.phase == Phase::AwaitingDraw) return seatIndex == state.currentTurn ? std::vector<LegalAction>{{ActionType::Draw}} : std::vector<LegalAction>{};
  if (state.phase == Phase::AwaitingClaims) {
    if (!state.lastDiscard || state.lastDiscard->bySeat == seatIndex) return {};
    const auto player = getPlayer(state, seatIndex);
    std::vector<LegalAction> actions{{ActionType::Pass}};
    auto claimTiles = player.concealedTiles;
    claimTiles.push_back(state.lastDiscard->tile);
    if (validateWinningHand(claimTiles, player.melds, player.flowers).isWin &&
        candidateWinMeetsMinimumFan(state, seatIndex, WinSource::Discard, state.lastDiscard->tile)) {
      LegalAction win{ActionType::Win};
      win.source = WinSource::Discard;
      win.claimedTileId = state.lastDiscard->tile.id;
      actions.push_back(win);
    }
    if (seatIndex == nextSeatIndex(state.lastDiscard->bySeat)) {
      for (const auto& keys : getChowWaitTileKeys(state.lastDiscard->tile)) {
        std::vector<Tile> selected;
        for (const auto& key : keys) {
          const auto it = std::find_if(player.concealedTiles.begin(), player.concealedTiles.end(), [&](const Tile& candidate) {
            return candidate.key == key && std::none_of(selected.begin(), selected.end(), [&](const Tile& used) { return used.id == candidate.id; });
          });
          if (it != player.concealedTiles.end()) selected.push_back(*it);
        }
        if (selected.size() == 2) {
          LegalAction action{ActionType::Chow};
          action.tiles = tupleIds(selected, 2);
          action.claimedTileId = state.lastDiscard->tile.id;
          actions.push_back(action);
        }
      }
    }
    std::vector<Tile> matching;
    for (const auto& tile : player.concealedTiles) if (tile.key == state.lastDiscard->tile.key) matching.push_back(tile);
    if (matching.size() >= 2) {
      LegalAction pong{ActionType::Pong};
      pong.tiles = tupleIds(matching, 2);
      pong.claimedTileId = state.lastDiscard->tile.id;
      actions.push_back(pong);
    }
    if (matching.size() >= 3) {
      LegalAction kong{ActionType::Kong};
      kong.kongType = KongType::Exposed;
      kong.tiles = tupleIds(matching, 3);
      kong.claimedTileId = state.lastDiscard->tile.id;
      actions.push_back(kong);
    }
    return actions;
  }
  if (state.phase != Phase::AwaitingDiscard || seatIndex != state.currentTurn) return {};
  const auto player = getPlayer(state, seatIndex);
  std::vector<LegalAction> actions;
  for (const auto& tile : player.concealedTiles) {
    LegalAction action{ActionType::Discard};
    action.tileId = tile.id;
    actions.push_back(action);
  }
  const auto win = validateWinningHand(player.concealedTiles, player.melds, player.flowers);
  const auto winningTile = win.kind == "flowerWin"
    ? (!player.flowers.empty() ? std::optional<Tile>{player.flowers.back()} : std::nullopt)
    : (state.lastDraw ? std::optional<Tile>{state.lastDraw->tile} : (!player.concealedTiles.empty() ? std::optional<Tile>{player.concealedTiles.back()} : std::nullopt));
  const auto winSource = win.kind == "flowerWin" ? WinSource::Flower : WinSource::SelfDraw;
  if (win.isWin && candidateWinMeetsMinimumFan(state, seatIndex, winSource, winningTile)) {
    LegalAction action{ActionType::Win};
    action.source = winSource;
    actions.push_back(action);
  }
  std::map<std::string, std::vector<Tile>> groups;
  for (const auto& tile : player.concealedTiles) groups[tile.key].push_back(tile);
  for (const auto& [key, tiles] : groups) {
    if (tiles.size() >= 4) {
      LegalAction action{ActionType::Kong};
      action.kongType = KongType::Concealed;
      action.tileKey = key;
      action.tiles = tupleIds(tiles, 4);
      actions.push_back(action);
    }
  }
  for (const auto& meld : player.melds) {
    if (meld.kind != MeldKind::Pong || meld.tiles.empty()) continue;
    const auto key = meld.tiles.front().key;
    const auto it = std::find_if(player.concealedTiles.begin(), player.concealedTiles.end(), [&](const Tile& tile) { return tile.key == key; });
    if (it != player.concealedTiles.end()) {
      LegalAction action{ActionType::Kong};
      action.kongType = KongType::Added;
      action.tileKey = key;
      action.tileId = it->id;
      action.meldId = meld.id;
      actions.push_back(action);
    }
  }
  return actions;
}

RoundState drawTile(const RoundState& state, std::optional<int> seatIndex) {
  if (state.phase != Phase::AwaitingDraw) throw std::runtime_error("Cannot draw outside awaitingDraw");
  const int seat = seatIndex.value_or(state.currentTurn);
  if (seat != state.currentTurn) throw std::runtime_error("Not this seat's draw");
  if (state.wall.liveWall.empty()) return finishRoundAsExhaustiveDraw(state);
  auto next = state;
  auto result = drawLiveTileReplacingFlowers(getPlayer(state, seat), state.wall);
  next.phase = Phase::AwaitingDiscard;
  next.wall = result.wall;
  next.players = replacePlayer(next.players, seat, result.player);
  next.lastDraw.reset();
  if (result.drawnTile) next.lastDraw = LastDraw{*result.drawnTile, seat, "liveWall", state.turnNumber};
  return next;
}

RoundState discardTile(const RoundState& state, const std::string& tileId) {
  if (state.phase != Phase::AwaitingDiscard) throw std::runtime_error("Cannot discard outside awaitingDiscard");
  auto next = state;
  const int seat = state.currentTurn;
  auto player = getPlayer(state, seat);
  const auto it = std::find_if(player.concealedTiles.begin(), player.concealedTiles.end(), [&](const Tile& tile) { return tile.id == tileId; });
  if (it == player.concealedTiles.end()) throw std::runtime_error("Tile is not in concealed hand");
  const auto tile = *it;
  player.concealedTiles = removeTileIds(player.concealedTiles, {tileId});
  player.discards.push_back(tile);
  next.players = replacePlayer(next.players, seat, player);
  next.phase = Phase::AwaitingClaims;
  next.lastDraw.reset();
  next.lastDiscard = LastDiscard{tile, seat, state.turnNumber};
  return next;
}

RoundState passClaimWindow(const RoundState& state) {
  if (state.phase != Phase::AwaitingClaims || !state.lastDiscard) throw std::runtime_error("No claim window is open");
  auto next = state;
  next.phase = Phase::AwaitingDraw;
  next.currentTurn = nextSeatIndex(state.lastDiscard->bySeat);
  next.turnNumber = state.turnNumber + 1;
  next.lastDiscard.reset();
  return next;
}

RoundState claimDiscard(const RoundState& state, int seatIndex, const LegalAction& action) {
  if (state.phase != Phase::AwaitingClaims || !state.lastDiscard) throw std::runtime_error("No discard is available to claim");
  const auto legal = getLegalActions(state, seatIndex);
  if (std::none_of(legal.begin(), legal.end(), [&](const LegalAction& candidate) { return candidate == action; })) throw std::runtime_error("Illegal claim action");
  if (action.type == ActionType::Pass) return state;
  if (action.type == ActionType::Win) {
    RoundConclusion conclusion{ConclusionReason::Win, seatIndex, state.lastDiscard->tile, WinSource::Discard, "Seat " + std::to_string(seatIndex) + " wins on discard."};
    conclusion.responsibleSeat = state.lastDiscard->bySeat;
    return finish(state, conclusion);
  }
  if (action.type != ActionType::Chow && action.type != ActionType::Pong && !(action.type == ActionType::Kong && action.kongType == KongType::Exposed)) throw std::runtime_error("Unsupported claim action");
  const auto claimedTile = state.lastDiscard->tile;
  auto player = getPlayer(state, seatIndex);
  std::vector<Tile> usedTiles;
  for (const auto& id : action.tiles) {
    const auto it = std::find_if(player.concealedTiles.begin(), player.concealedTiles.end(), [&](const Tile& tile) { return tile.id == id; });
    if (it == player.concealedTiles.end()) throw std::runtime_error("Tile not found for claim");
    usedTiles.push_back(*it);
  }
  std::vector<Tile> meldTiles = usedTiles;
  meldTiles.push_back(claimedTile);
  const auto kind = action.type == ActionType::Kong ? MeldKind::ExposedKong : action.type == ActionType::Pong ? MeldKind::Pong : MeldKind::Chow;
  auto players = removeLastDiscardFromOwner(state);
  player = players[static_cast<std::size_t>(seatIndex)];
  player.concealedTiles = removeTileIds(player.concealedTiles, action.tiles);
  player.melds.push_back(makeMeld(kind, meldTiles, claimedTile.id, state.lastDiscard->bySeat));
  players[static_cast<std::size_t>(seatIndex)] = player;
  RoundState next = state;
  next.lastDiscard.reset();
  next.phase = Phase::AwaitingDiscard;
  next.currentTurn = seatIndex;
  next.turnNumber = state.turnNumber + 1;
  next.players = players;
  if (kind == MeldKind::ExposedKong) {
    auto result = drawKongReplacementReplacingFlowers(getPlayer(next, seatIndex), next.wall);
    next.wall = result.wall;
    next.players = replacePlayer(next.players, seatIndex, result.player);
    if (result.drawnTile) next.lastDraw = LastDraw{*result.drawnTile, seatIndex, "replacement", next.turnNumber};
  }
  return next;
}

RoundState declareKong(const RoundState& state, const std::string& tileKey, const std::string& meldId) {
  if (state.phase != Phase::AwaitingDiscard) throw std::runtime_error("Cannot declare kong outside awaitingDiscard");
  auto next = state;
  const int seat = state.currentTurn;
  auto player = getPlayer(state, seat);
  if (!meldId.empty()) {
    auto meldIt = std::find_if(player.melds.begin(), player.melds.end(), [&](const Meld& meld) { return meld.id == meldId; });
    auto tileIt = std::find_if(player.concealedTiles.begin(), player.concealedTiles.end(), [&](const Tile& tile) { return tile.key == tileKey; });
    if (meldIt == player.melds.end() || meldIt->kind != MeldKind::Pong || tileIt == player.concealedTiles.end() || primaryKey(*meldIt) != tileKey) throw std::runtime_error("Cannot add kong");
    auto tiles = meldIt->tiles;
    tiles.push_back(*tileIt);
    auto upgraded = makeMeld(MeldKind::AddedKong, tiles, meldIt->claimedTileId.value_or(""), meldIt->fromSeat);
    player.concealedTiles = removeTileIds(player.concealedTiles, {tileIt->id});
    *meldIt = upgraded;
    auto result = drawKongReplacementReplacingFlowers(player, state.wall);
    next.wall = result.wall;
    next.players = replacePlayer(next.players, seat, result.player);
    next.lastDraw.reset();
    if (result.drawnTile) next.lastDraw = LastDraw{*result.drawnTile, seat, "replacement", state.turnNumber};
    return next;
  }
  auto tiles = matchingTiles(player.concealedTiles, tileKey, 4);
  player.concealedTiles = removeTileIds(player.concealedTiles, tupleIds(tiles, 4));
  player.melds.push_back(makeMeld(MeldKind::ConcealedKong, tiles));
  auto result = drawKongReplacementReplacingFlowers(player, state.wall);
  next.wall = result.wall;
  next.players = replacePlayer(next.players, seat, result.player);
  next.lastDraw.reset();
  if (result.drawnTile) next.lastDraw = LastDraw{*result.drawnTile, seat, "replacement", state.turnNumber};
  return next;
}

RoundState declareSelfDrawWin(const RoundState& state) {
  if (state.phase != Phase::AwaitingDiscard) throw std::runtime_error("Cannot declare win outside awaitingDiscard");
  const auto legal = getLegalActions(state, state.currentTurn);
  const auto action = std::find_if(legal.begin(), legal.end(), [](const LegalAction& candidate) { return candidate.type == ActionType::Win; });
  if (action == legal.end()) throw std::runtime_error("Current player does not have a legal win");
  const auto player = getPlayer(state, state.currentTurn);
  std::optional<Tile> winningTile;
  if (action->source == WinSource::Flower && !player.flowers.empty()) winningTile = player.flowers.back();
  else if (state.lastDraw) winningTile = state.lastDraw->tile;
  else if (!player.concealedTiles.empty()) winningTile = player.concealedTiles.back();
  return finish(state, RoundConclusion{ConclusionReason::Win, state.currentTurn, winningTile, action->source, "Seat " + std::to_string(state.currentTurn) + " wins by " + (action->source == WinSource::Flower ? "flower" : "selfDraw") + "."});
}

RoundState finishRoundAsExhaustiveDraw(const RoundState& state) {
  return finish(state, RoundConclusion{ConclusionReason::ExhaustiveDraw, std::nullopt, std::nullopt, std::nullopt, "The live wall is exhausted."});
}

HongKongRules defaultHongKongRules() {
  HongKongRules rules;
  rules.minFan = 3;
  rules.paymentTable = {
    {0, 0, 1},
    {1, 1, 2},
    {2, 2, 4},
    {3, 3, 8},
    {4, 4, 16},
    {5, 5, 24},
    {6, 6, 32},
    {7, 7, 48},
    {8, 8, 64},
    {9, 9, 96},
    {10, 10, 128},
    {11, 11, 192},
    {12, 12, 256},
    {13, std::nullopt, 384}
  };
  auto add = [&](std::string id, std::string name, int fan, std::string group = "", std::vector<std::string> replaces = {}) {
    rules.fanTable[id] = FanFeatureRule{name, fan, true, "", group, replaces};
  };
  add("seat-flower", "Seat flower", 1);
  add("seat-season", "Seat season", 1);
  add("no-bonus-tiles", "No flowers or seasons", 1);
  add("dragon-pong", "Dragon Pong/Kong", 1);
  add("seat-wind-pong", "Seat wind Pong/Kong", 1);
  add("round-wind-pong", "Round wind Pong/Kong", 1);
  add("all-chows", "All Chows", 1);
  add("all-pongs", "All Pongs", 3);
  add("mixed-one-suit", "Mixed One Suit", 3, "suit-pattern");
  add("pure-one-suit", "Pure One Suit", 7, "suit-pattern");
  add("little-three-dragons", "Little Three Dragons", 4, "", {"dragon-pong"});
  add("big-three-dragons", "Big Three Dragons", 8, "", {"dragon-pong", "little-three-dragons"});
  add("little-four-winds", "Little Four Winds", 6, "", {"seat-wind-pong", "round-wind-pong"});
  add("big-four-winds", "Big Four Winds", 13, "", {"seat-wind-pong", "round-wind-pong", "little-four-winds"});
  add("seven-pairs", "Seven Pairs", 4);
  add("thirteen-orphans", "Thirteen Orphans", 13);
  add("all-honours", "All Honours", 10, "terminal-honour-pattern");
  add("all-terminals", "All Terminals", 10, "terminal-honour-pattern");
  add("all-terminals-and-honours", "All Terminals and Honours", 13, "terminal-honour-pattern");
  return rules;
}

int lookupBasePayment(int fan, const std::vector<PaymentBand>& paymentTable) {
  const int normalized = std::max(0, fan);
  for (const auto& band : paymentTable) {
    const int max = band.maxFan.value_or(std::numeric_limits<int>::max());
    if (normalized >= band.minFan && normalized <= max) return band.points;
  }
  throw std::runtime_error("No payment table entry matches fan");
}

PaymentResult calculatePayments(int fan, Wind winner, const std::string& winType, std::optional<Wind> responsiblePayer, int minFan) {
  const auto rules = defaultHongKongRules();
  PaymentResult result;
  result.fan = fan;
  result.minFan = minFan;
  result.basePoints = lookupBasePayment(fan, rules.paymentTable);
  for (const auto wind : kWinds) result.deltas[wind] = 0;
  if (fan < minFan) return result;
  result.eligible = true;
  struct PayerCharge {
    Wind payer{};
    int multiplier{1};
    std::string reason;
  };
  std::vector<PayerCharge> charges;
  if (winType == "discard") {
    if (!responsiblePayer) throw std::runtime_error("Discard wins require a discarder");
    if (*responsiblePayer == winner) throw std::runtime_error("Winner cannot pay their own discard win");
    charges.push_back({*responsiblePayer, 1, "discarder pays"});
  } else if (winType == "rob-kong") {
    if (!responsiblePayer) throw std::runtime_error("Robbing a Kong requires a responsible Kong player");
    if (*responsiblePayer == winner) throw std::runtime_error("Winner cannot pay their own robbed Kong");
    charges.push_back({*responsiblePayer, 3, "robbed Kong player pays for all players"});
  } else if (winType == "self-pick-all-called") {
    if (!responsiblePayer) throw std::runtime_error("All-called self-draw requires a responsible final meld provider");
    if (*responsiblePayer == winner) throw std::runtime_error("Winner cannot pay their own all-called self-draw");
    charges.push_back({*responsiblePayer, 3, "final meld provider pays for all players"});
  } else {
    for (const auto payer : kWinds) {
      if (payer != winner) charges.push_back({payer, 1, "self-draw payer"});
    }
  }
  for (const auto& charge : charges) {
    RoundPaymentLine line;
    line.from = charge.payer;
    line.to = winner;
    line.basePoints = result.basePoints;
    line.doublings = 0;
    line.points = result.basePoints * charge.multiplier;
    line.reasons.push_back(charge.reason);
    if (charge.multiplier == 3) line.reasons.push_back("pays equivalent of three players");
    result.deltas[charge.payer] -= line.points;
    result.deltas[winner] += line.points;
    result.lines.push_back(line);
  }
  return result;
}

RoundSettlement scoreWinningRound(const RoundState& state, const RoundConclusion& conclusion) {
  if (conclusion.reason != ConclusionReason::Win || !conclusion.winnerSeat || !conclusion.source) throw std::runtime_error("Cannot score non-winning round");
  const auto rules = defaultHongKongRules();
  const auto winner = getPlayer(state, *conclusion.winnerSeat);
  auto concealed = winner.concealedTiles;
  if ((*conclusion.source == WinSource::Discard || *conclusion.source == WinSource::RobbingKong) && conclusion.winningTile) concealed.push_back(*conclusion.winningTile);
  std::vector<Tile> allTiles = concealed;
  for (const auto& meld : winner.melds) allTiles.insert(allTiles.end(), meld.tiles.begin(), meld.tiles.end());
  const auto validation = validateWinningHand(concealed, winner.melds, winner.flowers);
  auto melds = winner.melds;
  if (validation.kind == "standard") {
    auto concealedMelds = decomposeStandardConcealedMelds(concealed, static_cast<int>(winner.melds.size()));
    melds.insert(melds.end(), concealedMelds.begin(), concealedMelds.end());
  }

  std::vector<FanOccurrence> features;
  std::map<std::string, Wind> flowerSeats{{"flower-plum", Wind::East}, {"flower-orchid", Wind::South}, {"flower-chrysanthemum", Wind::West}, {"flower-bamboo", Wind::North}, {"season-spring", Wind::East}, {"season-summer", Wind::South}, {"season-autumn", Wind::West}, {"season-winter", Wind::North}};
  for (const auto& tile : winner.flowers) {
    if (flowerSeats.contains(tile.key) && flowerSeats[tile.key] == winner.wind) addFeature(features, tile.category == Category::Flower ? "seat-flower" : "seat-season", rules, toString(winner.wind));
  }
  if (winner.flowers.empty()) addFeature(features, "no-bonus-tiles", rules);
  for (const auto& meld : melds) {
    if (!isPongLike(meld)) continue;
    const auto key = primaryKey(meld);
    if (hasKeyKind(key, Category::Dragon)) addFeature(features, "dragon-pong", rules, key);
    if (key == toString(winner.wind)) addFeature(features, "seat-wind-pong", rules, key);
    if (key == toString(state.prevailingWind)) addFeature(features, "round-wind-pong", rules, key);
  }
  if (melds.size() == 4 && std::all_of(melds.begin(), melds.end(), [](const Meld& meld) { return meld.kind == MeldKind::Chow; })) addFeature(features, "all-chows", rules);
  if (melds.size() == 4 && std::all_of(melds.begin(), melds.end(), isPongLike)) addFeature(features, "all-pongs", rules);

  std::set<std::string> dragonSets;
  std::set<std::string> windSets;
  for (const auto& meld : melds) {
    if (!isPongLike(meld)) continue;
    const auto key = primaryKey(meld);
    if (hasKeyKind(key, Category::Dragon)) dragonSets.insert(key);
    if (hasKeyKind(key, Category::Wind)) windSets.insert(key);
  }
  if (dragonSets.size() == 3) addFeature(features, "big-three-dragons", rules);
  if (windSets.size() == 4) addFeature(features, "big-four-winds", rules);

  std::vector<Tile> scoringTiles;
  std::copy_if(allTiles.begin(), allTiles.end(), std::back_inserter(scoringTiles), [](const Tile& tile) { return !isFlowerOrSeason(tile); });
  std::set<Suit> suits;
  bool hasHonours = false;
  bool allHonours = !scoringTiles.empty();
  bool allTerminals = !scoringTiles.empty();
  bool allTerminalHonours = !scoringTiles.empty();
  for (const auto& tile : scoringTiles) {
    if (tile.suit) suits.insert(*tile.suit);
    if (isHonorTile(tile)) hasHonours = true;
    allHonours = allHonours && isHonorTile(tile);
    allTerminals = allTerminals && tile.rank && (*tile.rank == 1 || *tile.rank == 9);
    allTerminalHonours = allTerminalHonours && (isHonorTile(tile) || (tile.rank && (*tile.rank == 1 || *tile.rank == 9)));
  }
  if (suits.size() == 1 && hasHonours) addFeature(features, "mixed-one-suit", rules);
  if (suits.size() == 1 && !hasHonours) addFeature(features, "pure-one-suit", rules);
  if (allHonours) addFeature(features, "all-honours", rules);
  if (allTerminals) addFeature(features, "all-terminals", rules);
  if (allTerminalHonours) addFeature(features, "all-terminals-and-honours", rules);
  if (validation.kind == "sevenPairs" || isSevenPairsShape(scoringTiles)) addFeature(features, "seven-pairs", rules);
  if (validation.kind == "thirteenOrphans" || isThirteenOrphansShape(scoringTiles)) addFeature(features, "thirteen-orphans", rules);

  RoundSettlement settlement;
  settlement.minFan = state.rules.minFan;
  std::vector<RoundFanFeature> excluded;
  const auto included = resolveIncluded(features, excluded);
  settlement.excludedFeatures = excluded;
  for (const auto& feature : included) {
    settlement.fan += feature.fan;
    settlement.includedFeatures.push_back({feature.id, feature.name, feature.fan, feature.source.empty() ? std::nullopt : std::optional<std::string>{feature.source}, std::nullopt});
  }
  std::string winType = "self-pick";
  std::optional<Wind> responsiblePayer;
  if (*conclusion.source == WinSource::Discard && state.lastDiscard) {
    winType = "discard";
    responsiblePayer = getPlayer(state, state.lastDiscard->bySeat).wind;
  } else if (*conclusion.source == WinSource::RobbingKong && conclusion.responsibleSeat) {
    winType = "rob-kong";
    responsiblePayer = getPlayer(state, *conclusion.responsibleSeat).wind;
  } else if ((*conclusion.source == WinSource::SelfDraw || *conclusion.source == WinSource::Flower) &&
             winner.melds.size() == 4 &&
             std::all_of(winner.melds.begin(), winner.melds.end(), [](const Meld& meld) {
               return meld.fromSeat.has_value() && !meld.concealed;
             }) &&
             winner.melds.back().fromSeat) {
    winType = "self-pick-all-called";
    responsiblePayer = getPlayer(state, *winner.melds.back().fromSeat).wind;
  }
  auto payment = calculatePayments(settlement.fan, winner.wind, winType, responsiblePayer, state.rules.minFan);
  settlement.eligible = payment.eligible;
  settlement.basePoints = payment.basePoints;
  settlement.paymentLines = payment.lines;
  settlement.deltas = payment.deltas;
  return settlement;
}

LegalAction selectAiAction(const RoundState& state, int seatIndex, const std::vector<LegalAction>& inputActions, AiDifficulty difficulty, const std::string& seed) {
  auto actions = inputActions.empty() ? getLegalActions(state, seatIndex) : inputActions;
  if (actions.empty()) return {};
  if (difficulty == AiDifficulty::Easy) {
    SeedRandom random(seed + ":" + std::to_string(seatIndex) + ":" + std::to_string(state.turnNumber));
    return actions[static_cast<std::size_t>(std::floor(random.next() * actions.size()))];
  }
  const auto nextRound = std::find_if(actions.begin(), actions.end(), [](const LegalAction& action) { return action.type == ActionType::NextRound; });
  if (nextRound != actions.end()) return *nextRound;
  const auto win = std::find_if(actions.begin(), actions.end(), [](const LegalAction& action) { return action.type == ActionType::Win; });
  if (win != actions.end()) return *win;
  const auto draw = std::find_if(actions.begin(), actions.end(), [](const LegalAction& action) { return action.type == ActionType::Draw; });
  if (draw != actions.end()) return *draw;
  if (state.phase == Phase::AwaitingDiscard) {
    const auto kong = std::find_if(actions.begin(), actions.end(), [](const LegalAction& action) { return action.type == ActionType::Kong; });
    if (kong != actions.end() && difficulty != AiDifficulty::Medium) return *kong;
    std::sort(actions.begin(), actions.end(), [&](const LegalAction& left, const LegalAction& right) {
      const auto delta = discardScore(right, state, seatIndex, difficulty) - discardScore(left, state, seatIndex, difficulty);
      return delta == 0 ? actionStableKey(left) < actionStableKey(right) : delta < 0;
    });
    return actions.front();
  }
  std::sort(actions.begin(), actions.end(), [&](const LegalAction& left, const LegalAction& right) {
    const auto delta = claimScore(right, state, difficulty) - claimScore(left, state, difficulty);
    return delta == 0 ? actionStableKey(left) < actionStableKey(right) : delta < 0;
  });
  return actions.front();
}

AiAdvanceResult advanceAiRound(const RoundState& state, AiDifficulty difficulty, const std::string& seed) {
  if (state.phase == Phase::Finished) {
    const auto actions = getLegalActions(state, state.dealerSeat);
    auto action = selectAiAction(state, state.dealerSeat, actions, difficulty, seed);
    return {applyLegalAction(state, state.dealerSeat, action), {{state.dealerSeat, difficulty, actions, action, true}}};
  }
  if (state.phase == Phase::AwaitingClaims) {
    std::vector<AiDecision> decisions;
    for (const auto& player : state.players) {
      if (state.lastDiscard && player.seatIndex == state.lastDiscard->bySeat) continue;
      const auto actions = getLegalActions(state, player.seatIndex);
      auto action = selectAiAction(state, player.seatIndex, actions, difficulty, seed);
      decisions.push_back({player.seatIndex, difficulty, actions, action, false});
    }
    auto best = decisions.end();
    for (auto it = decisions.begin(); it != decisions.end(); ++it) {
      if (!it->selectedAction || claimPriority(*it->selectedAction) == 0) continue;
      if (best == decisions.end() ||
          claimPriority(*it->selectedAction) > claimPriority(*best->selectedAction) ||
          (claimPriority(*it->selectedAction) == claimPriority(*best->selectedAction) && claimSeatDistance(state, it->seatIndex) < claimSeatDistance(state, best->seatIndex))) {
        best = it;
      }
    }
    if (best == decisions.end()) return {passClaimWindow(state), decisions};
    auto next = applyLegalAction(state, best->seatIndex, *best->selectedAction);
    for (auto& decision : decisions) decision.applied = decision.seatIndex == best->seatIndex;
    return {next, decisions};
  }
  const int seat = state.currentTurn;
  const auto actions = getLegalActions(state, seat);
  auto action = selectAiAction(state, seat, actions, difficulty, seed);
  return {applyLegalAction(state, seat, action), {{seat, difficulty, actions, action, true}}};
}

AiSimulationResult runAiRoundSimulation(const std::string& seed, AiDifficulty difficulty, int maxSteps) {
  auto initial = createInitialRoundState(seed);
  auto state = initial;
  std::vector<AiDecision> decisions;
  for (int step = 0; step < maxSteps; ++step) {
    if (state.phase == Phase::Finished) return {initial, state, step, true, decisions, ""};
    auto result = advanceAiRound(state, difficulty, seed);
    decisions.insert(decisions.end(), result.decisions.begin(), result.decisions.end());
    const bool anySelectedAction = std::any_of(result.decisions.begin(), result.decisions.end(), [](const AiDecision& decision) {
      return decision.selectedAction.has_value();
    });
    const bool noObservableProgress =
      result.state.phase == state.phase &&
      result.state.turnNumber == state.turnNumber &&
      result.state.currentTurn == state.currentTurn &&
      result.state.wall.liveWall.size() == state.wall.liveWall.size() &&
      result.state.wall.deadWall.size() == state.wall.deadWall.size() &&
      result.state.wall.replacementDraws.size() == state.wall.replacementDraws.size();
    if (!anySelectedAction && noObservableProgress) {
      return {initial, state, step, false, decisions, "No legal AI progress was possible."};
    }
    state = result.state;
  }
  return {initial, state, maxSteps, state.phase == Phase::Finished, decisions, state.phase == Phase::Finished ? "" : "Simulation reached maxSteps."};
}

} // namespace mahjong
