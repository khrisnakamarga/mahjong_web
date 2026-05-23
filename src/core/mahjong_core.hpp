#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace mahjong {

enum class Category { Suit, Wind, Dragon, Flower, Season };
enum class Suit { Dots, Bamboo, Characters };
enum class Wind { East, South, West, North };
enum class Dragon { Red, Green, White };
enum class Phase { AwaitingDraw, AwaitingDiscard, AwaitingClaims, Finished };
enum class Controller { Ai, Human };
enum class MeldKind { Chow, Pong, ExposedKong, ConcealedKong, AddedKong };
enum class WinSource { SelfDraw, Discard, Flower, RobbingKong };
enum class ConclusionReason { Win, ExhaustiveDraw, Aborted };
enum class ActionType { NextRound, Draw, Discard, Win, Chow, Pong, Kong, Pass };
enum class KongType { None, Exposed, Concealed, Added };
enum class AiDifficulty { Easy, Medium, Hard };

struct Tile {
  std::string key;
  Category category{};
  std::string name;
  std::string id;
  int copy{};
  std::optional<Suit> suit;
  std::optional<int> rank;
  std::optional<Wind> wind;
  std::optional<Dragon> dragon;
};

struct Meld {
  std::string id;
  MeldKind kind{};
  std::vector<Tile> tiles;
  std::optional<std::string> claimedTileId;
  std::optional<int> fromSeat;
  bool concealed{false};
};

struct PlayerState {
  int seatIndex{};
  Wind wind{};
  Controller controller{Controller::Ai};
  std::string displayName;
  int score{};
  std::vector<Tile> concealedTiles;
  std::vector<Tile> flowers;
  std::vector<Meld> melds;
  std::vector<Tile> discards;
};

struct WallState {
  std::string seed;
  std::vector<Tile> liveWall;
  std::vector<Tile> deadWall;
  std::vector<Tile> replacementDraws;
};

struct LastDiscard {
  Tile tile;
  int bySeat{};
  int turnNumber{};
};

struct LastDraw {
  Tile tile;
  int seatIndex{};
  std::string source;
  int turnNumber{};
};

struct RoundFanFeature {
  std::string id;
  std::string name;
  int fan{};
  std::optional<std::string> source;
  std::optional<std::string> replacedBy;
};

struct RoundPaymentLine {
  Wind from{};
  Wind to{};
  int basePoints{};
  int doublings{};
  int points{};
  std::vector<std::string> reasons;
};

struct RoundSettlement {
  int fan{};
  int minFan{};
  bool eligible{};
  int basePoints{};
  std::vector<RoundFanFeature> includedFeatures;
  std::vector<RoundFanFeature> excludedFeatures;
  std::vector<RoundPaymentLine> paymentLines;
  std::map<Wind, int> deltas;
};

struct RoundConclusion {
  ConclusionReason reason{};
  std::optional<int> winnerSeat;
  std::optional<Tile> winningTile;
  std::optional<WinSource> source;
  std::string message;
  std::optional<RoundSettlement> settlement;
  std::optional<int> responsibleSeat;
};

struct RoundRules {
  std::string name{"Hong Kong Mahjong bootstrap rules"};
  int minFan{3};
  int playerCount{4};
};

struct RoundState {
  Phase phase{Phase::AwaitingDiscard};
  RoundRules rules;
  int dealerSeat{};
  std::optional<int> windRoundStartDealerSeat;
  Wind prevailingWind{Wind::East};
  int currentTurn{};
  int turnNumber{1};
  std::vector<PlayerState> players;
  WallState wall;
  std::optional<LastDiscard> lastDiscard;
  std::optional<LastDraw> lastDraw;
  std::optional<RoundConclusion> conclusion;
};

struct LegalAction {
  ActionType type{};
  std::string tileId;
  WinSource source{WinSource::SelfDraw};
  std::string claimedTileId;
  std::vector<std::string> tiles;
  KongType kongType{KongType::None};
  std::string tileKey;
  std::string meldId;
};

struct FanFeatureRule {
  std::string name;
  int fan{};
  bool enabled{true};
  std::string description;
  std::string replacementGroup;
  std::vector<std::string> replaces;
};

struct PaymentBand {
  int minFan{};
  std::optional<int> maxFan;
  int points{};
};

struct HongKongRules {
  int minFan{3};
  std::map<std::string, FanFeatureRule> fanTable;
  std::vector<PaymentBand> paymentTable;
};

struct PaymentResult {
  int fan{};
  int minFan{};
  bool eligible{};
  int basePoints{};
  std::vector<RoundPaymentLine> lines;
  std::map<Wind, int> deltas;
};

struct WinValidationResult {
  bool isWin{};
  std::string kind;
  std::string reason;
};

struct AiDecision {
  int seatIndex{};
  AiDifficulty difficulty{AiDifficulty::Medium};
  std::vector<LegalAction> legalActions;
  std::optional<LegalAction> selectedAction;
  bool applied{};
};

struct AiAdvanceResult {
  RoundState state;
  std::vector<AiDecision> decisions;
};

struct AiSimulationResult {
  RoundState initialState;
  RoundState finalState;
  int steps{};
  bool completed{};
  std::vector<AiDecision> decisions;
  std::string blocker;
};

std::string toString(Wind wind);
std::string toString(Category category);
std::string toString(ActionType type);
std::string toString(Phase phase);
std::string tileFace(const Tile& tile);

bool operator==(const LegalAction& left, const LegalAction& right);

std::vector<Tile> createTileSet();
WallState generateWall(const std::string& seed = "default");
std::vector<Tile> sortTiles(std::vector<Tile> tiles);
bool isFlowerOrSeason(const Tile& tile);
bool isHonorTile(const Tile& tile);
bool isSuitTile(const Tile& tile);
int tileKeySortValue(const std::string& key);

RoundState createInitialRoundState(
  const std::string& seed = "local-round",
  int dealerSeat = 0,
  Wind prevailingWind = Wind::East,
  std::optional<int> windRoundStartDealerSeat = std::nullopt,
  RoundRules rules = {},
  std::vector<Controller> controllers = {},
  std::vector<std::string> displayNames = {},
  std::vector<int> scores = {}
);
RoundState createNextRoundState(const RoundState& state, const std::string& seed = "");

WinValidationResult validateWinningHand(
  const std::vector<Tile>& tiles,
  const std::vector<Meld>& melds = {},
  const std::vector<Tile>& flowers = {},
  bool allowSpecialHands = true,
  bool allowFlowerWins = true
);
bool isFlowerWinningHand(const std::vector<Tile>& flowers);
std::vector<std::vector<std::string>> getChowWaitTileKeys(const Tile& tile);

std::vector<LegalAction> getLegalActions(const RoundState& state, int seatIndex);
// Priority ordering for simultaneous claim resolution:
// 4 = Win, 3 = exposed Kong, 2 = Pong, 1 = Chow, 0 = anything else.
// Used by both AI claim arbitration and the server's human-claim precedence
// gate so a player cannot Chow on a discard that someone else can Pong/Win.
int claimPriority(const LegalAction& action);
RoundState drawTile(const RoundState& state, std::optional<int> seatIndex = std::nullopt);
RoundState discardTile(const RoundState& state, const std::string& tileId);
RoundState passClaimWindow(const RoundState& state);
RoundState claimDiscard(const RoundState& state, int seatIndex, const LegalAction& action);
RoundState declareKong(const RoundState& state, const std::string& tileKey, const std::string& meldId = "");
RoundState declareSelfDrawWin(const RoundState& state);
RoundState finishRoundAsExhaustiveDraw(const RoundState& state);

HongKongRules defaultHongKongRules();
int lookupBasePayment(int fan, const std::vector<PaymentBand>& paymentTable = defaultHongKongRules().paymentTable);
PaymentResult calculatePayments(int fan, Wind winner, const std::string& winType, std::optional<Wind> responsiblePayer = std::nullopt, int minFan = 3);
RoundSettlement scoreWinningRound(const RoundState& state, const RoundConclusion& conclusion);

LegalAction selectAiAction(const RoundState& state, int seatIndex, const std::vector<LegalAction>& legalActions = {}, AiDifficulty difficulty = AiDifficulty::Medium, const std::string& seed = "ai");
AiAdvanceResult advanceAiRound(const RoundState& state, AiDifficulty difficulty = AiDifficulty::Medium, const std::string& seed = "ai-round");
AiSimulationResult runAiRoundSimulation(const std::string& seed = "ai-round", AiDifficulty difficulty = AiDifficulty::Medium, int maxSteps = 1000);

} // namespace mahjong
