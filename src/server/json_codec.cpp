#include "server/json_codec.hpp"

#include <unordered_map>

namespace mahjong::server::codec {
namespace {

template <typename Enum>
std::string lookup(const std::unordered_map<Enum, std::string>& table, Enum value, const char* label) {
  const auto it = table.find(value);
  if (it == table.end()) return std::string("unknown_") + label;
  return it->second;
}

const std::unordered_map<Category, std::string>& categoryTable() {
  static const std::unordered_map<Category, std::string> table{
    {Category::Suit, "suit"},
    {Category::Wind, "wind"},
    {Category::Dragon, "dragon"},
    {Category::Flower, "flower"},
    {Category::Season, "season"},
  };
  return table;
}

const std::unordered_map<Suit, std::string>& suitTable() {
  static const std::unordered_map<Suit, std::string> table{
    {Suit::Dots, "dots"},
    {Suit::Bamboo, "bamboo"},
    {Suit::Characters, "characters"},
  };
  return table;
}

const std::unordered_map<Wind, std::string>& windTable() {
  static const std::unordered_map<Wind, std::string> table{
    {Wind::East, "east"},
    {Wind::South, "south"},
    {Wind::West, "west"},
    {Wind::North, "north"},
  };
  return table;
}

const std::unordered_map<Dragon, std::string>& dragonTable() {
  static const std::unordered_map<Dragon, std::string> table{
    {Dragon::Red, "red"},
    {Dragon::Green, "green"},
    {Dragon::White, "white"},
  };
  return table;
}

const std::unordered_map<Phase, std::string>& phaseTable() {
  static const std::unordered_map<Phase, std::string> table{
    {Phase::AwaitingDraw, "awaiting_draw"},
    {Phase::AwaitingDiscard, "awaiting_discard"},
    {Phase::AwaitingClaims, "awaiting_claims"},
    {Phase::Finished, "finished"},
  };
  return table;
}

const std::unordered_map<Controller, std::string>& controllerTable() {
  static const std::unordered_map<Controller, std::string> table{
    {Controller::Ai, "ai"},
    {Controller::Human, "human"},
  };
  return table;
}

const std::unordered_map<MeldKind, std::string>& meldKindTable() {
  static const std::unordered_map<MeldKind, std::string> table{
    {MeldKind::Chow, "chow"},
    {MeldKind::Pong, "pong"},
    {MeldKind::ExposedKong, "exposed_kong"},
    {MeldKind::ConcealedKong, "concealed_kong"},
    {MeldKind::AddedKong, "added_kong"},
  };
  return table;
}

const std::unordered_map<WinSource, std::string>& winSourceTable() {
  static const std::unordered_map<WinSource, std::string> table{
    {WinSource::SelfDraw, "self_draw"},
    {WinSource::Discard, "discard"},
    {WinSource::Flower, "flower"},
    {WinSource::RobbingKong, "robbing_kong"},
  };
  return table;
}

const std::unordered_map<ConclusionReason, std::string>& reasonTable() {
  static const std::unordered_map<ConclusionReason, std::string> table{
    {ConclusionReason::Win, "win"},
    {ConclusionReason::ExhaustiveDraw, "exhaustive_draw"},
    {ConclusionReason::Aborted, "aborted"},
  };
  return table;
}

const std::unordered_map<ActionType, std::string>& actionTypeTable() {
  static const std::unordered_map<ActionType, std::string> table{
    {ActionType::NextRound, "next_round"},
    {ActionType::Draw, "draw"},
    {ActionType::Discard, "discard"},
    {ActionType::Win, "win"},
    {ActionType::Chow, "chow"},
    {ActionType::Pong, "pong"},
    {ActionType::Kong, "kong"},
    {ActionType::Pass, "pass"},
  };
  return table;
}

const std::unordered_map<KongType, std::string>& kongTypeTable() {
  static const std::unordered_map<KongType, std::string> table{
    {KongType::None, "none"},
    {KongType::Exposed, "exposed"},
    {KongType::Concealed, "concealed"},
    {KongType::Added, "added"},
  };
  return table;
}

template <typename Enum>
std::optional<Enum> reverseLookup(const std::unordered_map<Enum, std::string>& table, const std::string& value) {
  for (const auto& [key, label] : table) {
    if (label == value) return key;
  }
  return std::nullopt;
}

} // namespace

std::string encode(Category category) { return lookup(categoryTable(), category, "category"); }
std::string encode(Suit suit) { return lookup(suitTable(), suit, "suit"); }
std::string encode(Wind wind) { return lookup(windTable(), wind, "wind"); }
std::string encode(Dragon dragon) { return lookup(dragonTable(), dragon, "dragon"); }
std::string encode(Phase phase) { return lookup(phaseTable(), phase, "phase"); }
std::string encode(Controller controller) { return lookup(controllerTable(), controller, "controller"); }
std::string encode(MeldKind kind) { return lookup(meldKindTable(), kind, "meld"); }
std::string encode(WinSource source) { return lookup(winSourceTable(), source, "win_source"); }
std::string encode(ConclusionReason reason) { return lookup(reasonTable(), reason, "reason"); }
std::string encode(ActionType type) { return lookup(actionTypeTable(), type, "action"); }
std::string encode(KongType kongType) { return lookup(kongTypeTable(), kongType, "kong"); }

std::optional<ActionType> decodeActionType(const std::string& value) {
  return reverseLookup(actionTypeTable(), value);
}
std::optional<KongType> decodeKongType(const std::string& value) {
  return reverseLookup(kongTypeTable(), value);
}
std::optional<WinSource> decodeWinSource(const std::string& value) {
  return reverseLookup(winSourceTable(), value);
}

json toJson(const Tile& tile) {
  json out{
    {"key", tile.key},
    {"category", encode(tile.category)},
    {"name", tile.name},
    {"id", tile.id},
    {"copy", tile.copy},
  };
  if (tile.suit) out["suit"] = encode(*tile.suit);
  if (tile.rank) out["rank"] = *tile.rank;
  if (tile.wind) out["wind"] = encode(*tile.wind);
  if (tile.dragon) out["dragon"] = encode(*tile.dragon);
  return out;
}

json toJson(const Meld& meld) {
  json tiles = json::array();
  for (const auto& tile : meld.tiles) tiles.push_back(toJson(tile));
  json out{
    {"id", meld.id},
    {"kind", encode(meld.kind)},
    {"tiles", std::move(tiles)},
    {"concealed", meld.concealed},
  };
  if (meld.claimedTileId) out["claimedTileId"] = *meld.claimedTileId;
  if (meld.fromSeat) out["fromSeat"] = *meld.fromSeat;
  return out;
}

json toJson(const LastDiscard& discard) {
  return json{
    {"tile", toJson(discard.tile)},
    {"bySeat", discard.bySeat},
    {"turnNumber", discard.turnNumber},
  };
}

json toJson(const LastDraw& draw) {
  return json{
    {"tile", toJson(draw.tile)},
    {"seatIndex", draw.seatIndex},
    {"source", draw.source},
    {"turnNumber", draw.turnNumber},
  };
}

json toJson(const RoundFanFeature& feature) {
  json out{
    {"id", feature.id},
    {"name", feature.name},
    {"fan", feature.fan},
  };
  if (feature.source) out["source"] = *feature.source;
  if (feature.replacedBy) out["replacedBy"] = *feature.replacedBy;
  return out;
}

json toJson(const RoundPaymentLine& line) {
  return json{
    {"from", encode(line.from)},
    {"to", encode(line.to)},
    {"basePoints", line.basePoints},
    {"doublings", line.doublings},
    {"points", line.points},
    {"reasons", line.reasons},
  };
}

json toJson(const RoundSettlement& settlement) {
  json includedFeatures = json::array();
  for (const auto& feature : settlement.includedFeatures) includedFeatures.push_back(toJson(feature));
  json excludedFeatures = json::array();
  for (const auto& feature : settlement.excludedFeatures) excludedFeatures.push_back(toJson(feature));
  json paymentLines = json::array();
  for (const auto& line : settlement.paymentLines) paymentLines.push_back(toJson(line));
  json deltas = json::object();
  for (const auto& [wind, value] : settlement.deltas) deltas[encode(wind)] = value;
  return json{
    {"fan", settlement.fan},
    {"minFan", settlement.minFan},
    {"eligible", settlement.eligible},
    {"basePoints", settlement.basePoints},
    {"includedFeatures", std::move(includedFeatures)},
    {"excludedFeatures", std::move(excludedFeatures)},
    {"paymentLines", std::move(paymentLines)},
    {"deltas", std::move(deltas)},
  };
}

json toJson(const RoundConclusion& conclusion) {
  json out{
    {"reason", encode(conclusion.reason)},
    {"message", conclusion.message},
  };
  if (conclusion.winnerSeat) out["winnerSeat"] = *conclusion.winnerSeat;
  if (conclusion.winningTile) out["winningTile"] = toJson(*conclusion.winningTile);
  if (conclusion.source) out["source"] = encode(*conclusion.source);
  if (conclusion.settlement) out["settlement"] = toJson(*conclusion.settlement);
  if (conclusion.responsibleSeat) out["responsibleSeat"] = *conclusion.responsibleSeat;
  return out;
}

json toJson(const LegalAction& action) {
  json out{
    {"type", encode(action.type)},
    {"tileId", action.tileId},
    {"source", encode(action.source)},
    {"claimedTileId", action.claimedTileId},
    {"tiles", action.tiles},
    {"kongType", encode(action.kongType)},
    {"tileKey", action.tileKey},
    {"meldId", action.meldId},
  };
  return out;
}

json toJson(const PublicPlayerSnapshot& player) {
  json flowers = json::array();
  for (const auto& tile : player.flowers) flowers.push_back(toJson(tile));
  json melds = json::array();
  for (const auto& meld : player.melds) melds.push_back(toJson(meld));
  json discards = json::array();
  for (const auto& tile : player.discards) discards.push_back(toJson(tile));
  json out{
    {"seatIndex", player.seatIndex},
    {"wind", encode(player.wind)},
    {"controller", encode(player.controller)},
    {"displayName", player.displayName},
    {"score", player.score},
    {"concealedCount", player.concealedCount},
    {"flowers", std::move(flowers)},
    {"melds", std::move(melds)},
    {"discards", std::move(discards)},
  };
  if (player.concealedTiles) {
    json tiles = json::array();
    for (const auto& tile : *player.concealedTiles) tiles.push_back(toJson(tile));
    out["concealedTiles"] = std::move(tiles);
  }
  return out;
}

json toJson(const RoomSeatRecord& seat) {
  return json{
    {"seatIndex", seat.seatIndex},
    {"wind", encode(seat.wind)},
    {"controller", encode(seat.controller)},
    {"displayName", seat.displayName},
    {"connected", seat.connected},
  };
}

json toJson(const RoomSnapshot& snapshot) {
  json seats = json::array();
  for (const auto& seat : snapshot.seats) seats.push_back(toJson(seat));
  json players = json::array();
  for (const auto& player : snapshot.players) players.push_back(toJson(player));
  json legalActions = json::array();
  for (const auto& action : snapshot.legalActions) legalActions.push_back(toJson(action));
  json winHistory = json::array();
  for (const auto& conclusion : snapshot.winHistory) winHistory.push_back(toJson(conclusion));
  json out{
    {"roomCode", snapshot.roomCode},
    {"version", snapshot.version},
    {"phase", encode(snapshot.phase)},
    {"dealerSeat", snapshot.dealerSeat},
    {"prevailingWind", encode(snapshot.prevailingWind)},
    {"currentTurn", snapshot.currentTurn},
    {"turnNumber", snapshot.turnNumber},
    {"liveWallCount", snapshot.liveWallCount},
    {"deadWallCount", snapshot.deadWallCount},
    {"replacementDrawCount", snapshot.replacementDrawCount},
    {"aiDelayMs", snapshot.aiDelayMs},
    {"autoPass", snapshot.autoPass},
    {"minFan", snapshot.minFan},
    {"seats", std::move(seats)},
    {"players", std::move(players)},
    {"legalActions", std::move(legalActions)},
    {"winHistory", std::move(winHistory)},
  };
  if (snapshot.viewerSeatIndex) out["viewerSeatIndex"] = *snapshot.viewerSeatIndex;
  if (snapshot.lastDiscard) out["lastDiscard"] = toJson(*snapshot.lastDiscard);
  if (snapshot.lastDraw) out["lastDraw"] = toJson(*snapshot.lastDraw);
  if (snapshot.conclusion) out["conclusion"] = toJson(*snapshot.conclusion);
  return out;
}

json toJson(const ClaimLink& link) {
  return json{
    {"seatIndex", link.seatIndex},
    {"token", link.token},
    {"url", link.url},
  };
}

namespace {
template <typename T>
T valueOr(const json& object, const std::string& key, const T& fallback) {
  if (!object.contains(key) || object.at(key).is_null()) return fallback;
  return object.at(key).get<T>();
}
} // namespace

std::optional<LegalAction> legalActionFromJson(const json& value) {
  if (!value.is_object()) return std::nullopt;
  if (!value.contains("type")) return std::nullopt;
  const auto typeText = value.at("type").get<std::string>();
  const auto type = decodeActionType(typeText);
  if (!type) return std::nullopt;
  LegalAction action;
  action.type = *type;
  action.tileId = valueOr<std::string>(value, "tileId", "");
  if (value.contains("source") && value.at("source").is_string()) {
    if (auto source = decodeWinSource(value.at("source").get<std::string>())) action.source = *source;
  }
  action.claimedTileId = valueOr<std::string>(value, "claimedTileId", "");
  if (value.contains("tiles") && value.at("tiles").is_array()) {
    for (const auto& entry : value.at("tiles")) {
      if (entry.is_string()) action.tiles.push_back(entry.get<std::string>());
    }
  }
  if (value.contains("kongType") && value.at("kongType").is_string()) {
    if (auto kong = decodeKongType(value.at("kongType").get<std::string>())) action.kongType = *kong;
  }
  action.tileKey = valueOr<std::string>(value, "tileKey", "");
  action.meldId = valueOr<std::string>(value, "meldId", "");
  return action;
}

} // namespace mahjong::server::codec
