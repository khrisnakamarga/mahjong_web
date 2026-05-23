#include "core/mahjong_core.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string controllerLabel(mahjong::Controller controller) {
  return controller == mahjong::Controller::Human ? "human" : "ai";
}

std::string sourceLabel(mahjong::WinSource source) {
  if (source == mahjong::WinSource::Discard) return "discard";
  if (source == mahjong::WinSource::Flower) return "flower";
  if (source == mahjong::WinSource::RobbingKong) return "robbing kong";
  return "self-draw";
}

std::string tileLabel(const mahjong::Tile& tile) {
  return mahjong::tileFace(tile) + " " + tile.name;
}

std::optional<mahjong::Tile> findTileById(const mahjong::RoundState& state, const std::string& tileId) {
  for (const auto& player : state.players) {
    for (const auto& tile : player.concealedTiles) if (tile.id == tileId) return tile;
    for (const auto& tile : player.flowers) if (tile.id == tileId) return tile;
    for (const auto& tile : player.discards) if (tile.id == tileId) return tile;
    for (const auto& meld : player.melds) {
      for (const auto& tile : meld.tiles) if (tile.id == tileId) return tile;
    }
  }
  if (state.lastDiscard && state.lastDiscard->tile.id == tileId) return state.lastDiscard->tile;
  return std::nullopt;
}

std::string actionLabel(const mahjong::LegalAction& action, const mahjong::RoundState& state) {
  if (action.type == mahjong::ActionType::NextRound) return "Start next round";
  if (action.type == mahjong::ActionType::Draw) return "Draw tile";
  if (action.type == mahjong::ActionType::Pass) return "Pass";
  if (action.type == mahjong::ActionType::Discard) {
    const auto tile = findTileById(state, action.tileId);
    return "Discard " + (tile ? tileLabel(*tile) : action.tileId);
  }
  if (action.type == mahjong::ActionType::Win) return "Declare win (" + sourceLabel(action.source) + ")";
  if (action.type == mahjong::ActionType::Chow) return "Chow";
  if (action.type == mahjong::ActionType::Pong) return "Pong";
  if (action.type == mahjong::ActionType::Kong) {
    if (action.kongType == mahjong::KongType::Concealed) return "Concealed Kong " + action.tileKey;
    if (action.kongType == mahjong::KongType::Added) return "Added Kong " + action.tileKey;
    return "Exposed Kong";
  }
  return "Action";
}

void printTileRow(const std::vector<mahjong::Tile>& tiles, bool numbered) {
  const auto sorted = mahjong::sortTiles(tiles);
  if (sorted.empty()) {
    std::cout << "none";
    return;
  }
  for (std::size_t index = 0; index < sorted.size(); ++index) {
    if (numbered) std::cout << "[" << index + 1 << "] ";
    std::cout << tileLabel(sorted[index]);
    if (index + 1 < sorted.size()) std::cout << " | ";
  }
}

void printSettlement(const mahjong::RoundState& state) {
  if (!state.conclusion) return;
  std::cout << "\nConclusion: " << state.conclusion->message << "\n";
  if (!state.conclusion->settlement) return;
  const auto& settlement = *state.conclusion->settlement;
  std::cout << "Fan: " << settlement.fan << " (minimum " << settlement.minFan << ")";
  if (!settlement.eligible) std::cout << " - below minimum, no payments";
  std::cout << "\nFeatures: ";
  if (settlement.includedFeatures.empty()) {
    std::cout << "none";
  } else {
    for (std::size_t index = 0; index < settlement.includedFeatures.size(); ++index) {
      const auto& feature = settlement.includedFeatures[index];
      std::cout << feature.name << " (" << feature.fan << ")";
      if (index + 1 < settlement.includedFeatures.size()) std::cout << ", ";
    }
  }
  std::cout << "\nPayments:\n";
  if (settlement.paymentLines.empty()) {
    std::cout << "  none\n";
  } else {
    for (const auto& line : settlement.paymentLines) {
      std::cout << "  " << mahjong::toString(line.from) << " pays " << mahjong::toString(line.to) << " " << line.points << "\n";
    }
  }
}

void renderTable(const mahjong::RoundState& state, std::optional<int> viewerSeat, bool revealAll) {
  std::cout << "\n=== Hong Kong Mahjong ===\n";
  std::cout << "Phase: " << mahjong::toString(state.phase)
            << " | Round wind: " << mahjong::toString(state.prevailingWind)
            << " | Dealer seat: " << state.dealerSeat
            << " | Current turn: " << state.currentTurn
            << " | Turn: " << state.turnNumber << "\n";
  std::cout << "Wall: " << state.wall.liveWall.size() << " live / " << state.wall.deadWall.size()
            << " dead / " << state.wall.replacementDraws.size() << " replacements\n";
  if (state.lastDiscard) {
    std::cout << "Last discard: " << tileLabel(state.lastDiscard->tile) << " by seat " << state.lastDiscard->bySeat << "\n";
  }
  for (const auto& player : state.players) {
    const bool isViewer = viewerSeat && *viewerSeat == player.seatIndex;
    const bool showHand = revealAll || state.phase == mahjong::Phase::Finished || isViewer;
    std::cout << "\nSeat " << player.seatIndex << " (" << mahjong::toString(player.wind) << ") "
              << player.displayName << " [" << controllerLabel(player.controller) << "]"
              << (player.seatIndex == state.currentTurn ? " <turn>" : "")
              << " score " << player.score << "\n";
    std::cout << "  Hand (" << player.concealedTiles.size() << "): ";
    if (showHand) printTileRow(player.concealedTiles, isViewer);
    else std::cout << std::string(player.concealedTiles.size(), '#');
    std::cout << "\n  Flowers/seasons: ";
    printTileRow(player.flowers, false);
    std::cout << "\n  Melds: ";
    if (player.melds.empty()) {
      std::cout << "none";
    } else {
      for (std::size_t index = 0; index < player.melds.size(); ++index) {
        std::cout << "meld" << index + 1 << "(";
        printTileRow(player.melds[index].tiles, false);
        std::cout << ")";
        if (index + 1 < player.melds.size()) std::cout << " | ";
      }
    }
    std::cout << "\n  Discards: ";
    printTileRow(player.discards, false);
    std::cout << "\n";
  }
  printSettlement(state);
}

mahjong::RoundState applyAction(const mahjong::RoundState& state, int seatIndex, const mahjong::LegalAction& action) {
  if (action.type == mahjong::ActionType::NextRound) return mahjong::createNextRoundState(state);
  if (action.type == mahjong::ActionType::Draw) return mahjong::drawTile(state, seatIndex);
  if (action.type == mahjong::ActionType::Discard) return mahjong::discardTile(state, action.tileId);
  if (action.type == mahjong::ActionType::Pass) return state.phase == mahjong::Phase::AwaitingClaims ? mahjong::passClaimWindow(state) : state;
  if (action.type == mahjong::ActionType::Win) {
    return action.source == mahjong::WinSource::Discard ? mahjong::claimDiscard(state, seatIndex, action) : mahjong::declareSelfDrawWin(state);
  }
  if (action.type == mahjong::ActionType::Kong && action.kongType != mahjong::KongType::Exposed) {
    return mahjong::declareKong(state, action.tileKey, action.meldId);
  }
  return mahjong::claimDiscard(state, seatIndex, action);
}

mahjong::RoundState advanceAiUntilHumanPrompt(mahjong::RoundState state, int humanSeat, mahjong::AiDifficulty difficulty, int maxSteps = 120) {
  for (int step = 0; step < maxSteps; ++step) {
    if (state.phase == mahjong::Phase::Finished || !mahjong::getLegalActions(state, humanSeat).empty()) return state;
    const auto before = state.turnNumber;
    auto advanced = mahjong::advanceAiRound(state, difficulty, state.wall.seed);
    state = advanced.state;
    if (advanced.decisions.empty() || (state.turnNumber == before && advanced.state.wall.liveWall.empty())) return state;
  }
  return state;
}

int readNumber(const std::string& prompt, int min, int max) {
  while (true) {
    std::cout << prompt;
    std::string line;
    if (!std::getline(std::cin, line)) return min;
    std::stringstream stream(line);
    int value{};
    if (stream >> value && value >= min && value <= max) return value;
    std::cout << "Enter a number from " << min << " to " << max << ".\n";
  }
}

mahjong::AiDifficulty chooseDifficulty() {
  std::cout << "AI difficulty: [1] easy [2] medium [3] hard\n";
  const int choice = readNumber("> ", 1, 3);
  if (choice == 1) return mahjong::AiDifficulty::Easy;
  if (choice == 3) return mahjong::AiDifficulty::Hard;
  return mahjong::AiDifficulty::Medium;
}

int chooseMinimumFan() {
  return readNumber("Minimum Fan to win (0-13, default Hong Kong minimum is 3)\n> ", 0, 13);
}

std::string timestampSeed(const std::string& prefix) {
  return prefix + "-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
}

void playLocalHumanGame() {
  const auto difficulty = chooseDifficulty();
  const int minFan = chooseMinimumFan();
  const int humanSeat = readNumber("Choose your seat: [0] east [1] south [2] west [3] north\n> ", 0, 3);
  std::vector<mahjong::Controller> controllers(4, mahjong::Controller::Ai);
  controllers[static_cast<std::size_t>(humanSeat)] = mahjong::Controller::Human;
  std::vector<std::string> names{"AI East", "AI South", "AI West", "AI North"};
  names[static_cast<std::size_t>(humanSeat)] = "You";
  mahjong::RoundRules rules;
  rules.minFan = minFan;
  auto state = mahjong::createInitialRoundState(timestampSeed("local-human"), 0, mahjong::Wind::East, std::nullopt, rules, controllers, names);
  bool revealAll = false;

  while (true) {
    state = advanceAiUntilHumanPrompt(state, humanSeat, difficulty);
    renderTable(state, humanSeat, revealAll);
    auto actions = mahjong::getLegalActions(state, humanSeat);
    if (state.phase == mahjong::Phase::Finished) {
      std::cout << "\n[n] next round, [r] reveal/hide, [q] quit\n> ";
      std::string line;
      if (!std::getline(std::cin, line) || line == "q") return;
      if (line == "r") {
        revealAll = !revealAll;
        continue;
      }
      if (line == "n") {
        state = mahjong::createNextRoundState(state);
        continue;
      }
      continue;
    }
    if (actions.empty()) {
      std::cout << "\nNo human action is available. Press Enter to let AI continue, or q to quit.\n> ";
      std::string line;
      if (!std::getline(std::cin, line) || line == "q") return;
      continue;
    }
    std::cout << "\nYour legal actions:\n";
    for (std::size_t index = 0; index < actions.size(); ++index) {
      std::cout << "  [" << index + 1 << "] " << actionLabel(actions[index], state) << "\n";
    }
    std::cout << "[r] reveal/hide all hands, [q] quit\n> ";
    std::string line;
    if (!std::getline(std::cin, line) || line == "q") return;
    if (line == "r") {
      revealAll = !revealAll;
      continue;
    }
    std::stringstream stream(line);
    std::size_t choice{};
    if (!(stream >> choice) || choice < 1 || choice > actions.size()) {
      std::cout << "Invalid action.\n";
      continue;
    }
    try {
      state = applyAction(state, humanSeat, actions[choice - 1]);
    } catch (const std::exception& error) {
      std::cout << "Action failed: " << error.what() << "\n";
    }
  }
}

void watchFourAi() {
  const auto difficulty = chooseDifficulty();
  const int minFan = chooseMinimumFan();
  mahjong::RoundRules rules;
  rules.minFan = minFan;
  auto state = mahjong::createInitialRoundState(timestampSeed("four-ai"), 0, mahjong::Wind::East, std::nullopt, rules);
  bool revealAll = true;
  while (true) {
    renderTable(state, std::nullopt, revealAll);
    std::cout << "\n[s] step, [a] auto to round end, [n] next round, [r] reveal/hide, [q] quit\n> ";
    std::string line;
    if (!std::getline(std::cin, line) || line == "q") return;
    if (line == "r") {
      revealAll = !revealAll;
    } else if (line == "n") {
      if (state.phase == mahjong::Phase::Finished) state = mahjong::createNextRoundState(state);
      else std::cout << "Round is not finished yet.\n";
    } else if (line == "s") {
      state = mahjong::advanceAiRound(state, difficulty, state.wall.seed).state;
    } else if (line == "a") {
      for (int step = 0; step < 1000 && state.phase != mahjong::Phase::Finished; ++step) {
        state = mahjong::advanceAiRound(state, difficulty, state.wall.seed).state;
      }
    }
  }
}

int runSimulationSmoke() {
  auto result = mahjong::runAiRoundSimulation("native-client-four-ai", mahjong::AiDifficulty::Medium, 1000);
  std::cout << "Hong Kong Mahjong native console client\n";
  std::cout << "Four-AI round completed: " << (result.completed ? "yes" : "no") << " in " << result.steps << " steps\n";
  std::cout << "Final phase: " << mahjong::toString(result.finalState.phase) << "\n";
  if (result.finalState.conclusion) {
    std::cout << result.finalState.conclusion->message << "\n";
  }
  return result.completed ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--simulate") return runSimulationSmoke();

  while (true) {
    std::cout << "\nHong Kong Mahjong C++\n";
    std::cout << "[1] Play local game against AI\n";
    std::cout << "[2] Watch four AI players\n";
    std::cout << "[3] Run one simulation smoke test\n";
    std::cout << "[0] Quit\n";
    const int choice = readNumber("> ", 0, 3);
    if (choice == 0) return 0;
    if (choice == 1) playLocalHumanGame();
    if (choice == 2) watchFourAi();
    if (choice == 3) {
      const int result = runSimulationSmoke();
      if (result != 0) return result;
    }
  }
}
