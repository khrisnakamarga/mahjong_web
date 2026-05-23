#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define _USE_MATH_DEFINES
#include <windows.h>

#include "core/mahjong_core.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr int kIdNewEast = 1001;
constexpr int kIdNewSouth = 1002;
constexpr int kIdNewWest = 1003;
constexpr int kIdNewNorth = 1004;
constexpr int kIdWatchAi = 1005;
constexpr int kIdStep = 1006;
constexpr int kIdAuto = 1007;
constexpr int kIdNextRound = 1008;
constexpr int kIdReveal = 1009;
constexpr int kIdDifficulty = 1010;
constexpr int kIdMinFan = 1011;
constexpr int kIdActionBase = 3000;
constexpr int kIdAutoToggle = 1012;
constexpr UINT_PTR kTimerAutoAi = 0x1;

constexpr int kToolbarHeight = 50;
constexpr int kStatusHeight = 24;
constexpr int kActionPanelWidth = 280;
constexpr int kActionButtonHeight = 30;
constexpr int kActionButtonGap = 6;

struct TileHit {
  RECT rect;
  std::string tileId;
};

struct AppState {
  mahjong::RoundState round;
  bool hasRound{false};
  bool watchMode{false};
  bool revealAll{false};
  std::optional<int> humanSeat;
  int viewerSeat{0};
  mahjong::AiDifficulty difficulty{mahjong::AiDifficulty::Medium};
  int minFan{3};
  std::vector<mahjong::LegalAction> currentActions;
  std::vector<HWND> actionButtons;
  HWND statusView{};
  HWND difficultyCombo{};
  HWND minFanEdit{};
  HFONT uiFont{};
  HFONT uiBoldFont{};
  HFONT tileBigFont{};
  HFONT tileNumberFont{};
  HFONT tileSmallFont{};
  HFONT tileIndexFont{};
  HFONT plaqueFont{};
  HFONT plaqueLargeFont{};
  HFONT centerLargeFont{};
  HFONT actionFont{};
  HBITMAP backbuffer{};
  int backbufferWidth{};
  int backbufferHeight{};
  std::vector<TileHit> handHits;
  std::optional<std::string> hoverTileId;
  std::string statusOverride;
  HWND autoAiButton{};
  bool autoAi{false};
};

AppState g_app;

std::string timestampSeed(const std::string& prefix) {
  return prefix + "-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
}

std::string sourceLabel(mahjong::WinSource source) {
  if (source == mahjong::WinSource::Discard) return "discard";
  if (source == mahjong::WinSource::Flower) return "flower";
  if (source == mahjong::WinSource::RobbingKong) return "robbing kong";
  return "self-draw";
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

std::string tileLabel(const mahjong::Tile& tile) {
  return mahjong::tileFace(tile) + " " + tile.name;
}

std::string actionLabel(const mahjong::LegalAction& action, const mahjong::RoundState& state) {
  if (action.type == mahjong::ActionType::NextRound) return "Start next round";
  if (action.type == mahjong::ActionType::Draw) return "Draw tile";
  if (action.type == mahjong::ActionType::Pass) return "Pass";
  if (action.type == mahjong::ActionType::Discard) {
    const auto tile = findTileById(state, action.tileId);
    return std::string("Discard ") + (tile ? tileLabel(*tile) : action.tileId);
  }
  if (action.type == mahjong::ActionType::Win) return std::string("Declare win (") + sourceLabel(action.source) + ")";
  if (action.type == mahjong::ActionType::Chow) return "Chow";
  if (action.type == mahjong::ActionType::Pong) return "Pong";
  if (action.type == mahjong::ActionType::Kong) {
    if (action.kongType == mahjong::KongType::Concealed) return "Concealed Kong " + action.tileKey;
    if (action.kongType == mahjong::KongType::Added) return "Added Kong " + action.tileKey;
    return "Exposed Kong";
  }
  return "Action";
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

void syncDifficulty() {
  const auto selected = static_cast<int>(SendMessageA(g_app.difficultyCombo, CB_GETCURSEL, 0, 0));
  if (selected == 0) g_app.difficulty = mahjong::AiDifficulty::Easy;
  else if (selected == 2) g_app.difficulty = mahjong::AiDifficulty::Hard;
  else g_app.difficulty = mahjong::AiDifficulty::Medium;
}

void syncMinimumFan() {
  if (!g_app.minFanEdit) return;
  char buffer[16]{};
  GetWindowTextA(g_app.minFanEdit, buffer, static_cast<int>(sizeof(buffer)));
  int parsed = std::atoi(buffer);
  parsed = std::clamp(parsed, 0, 13);
  g_app.minFan = parsed;
  if (g_app.hasRound) g_app.round.rules.minFan = parsed;
}

mahjong::RoundRules selectedRules() {
  mahjong::RoundRules rules;
  rules.minFan = g_app.minFan;
  return rules;
}

bool currentlyAwaitingHumanDiscard() {
  if (!g_app.hasRound || !g_app.humanSeat) return false;
  if (g_app.round.phase != mahjong::Phase::AwaitingDiscard) return false;
  if (g_app.round.currentTurn != *g_app.humanSeat) return false;
  for (const auto& action : g_app.currentActions) {
    if (action.type == mahjong::ActionType::Discard) return true;
  }
  return false;
}

bool tileIsDiscardable(const std::string& tileId) {
  for (const auto& action : g_app.currentActions) {
    if (action.type == mahjong::ActionType::Discard && action.tileId == tileId) return true;
  }
  return false;
}

// ---------- Drawing helpers ----------

void fillRect(HDC dc, RECT r, COLORREF color) {
  HBRUSH brush = CreateSolidBrush(color);
  FillRect(dc, &r, brush);
  DeleteObject(brush);
}

void fillRoundRect(HDC dc, RECT r, int radius, COLORREF fill, COLORREF border, int borderWidth = 1) {
  HBRUSH brush = CreateSolidBrush(fill);
  HPEN pen = CreatePen(PS_SOLID, borderWidth, border);
  HGDIOBJ oldBrush = SelectObject(dc, brush);
  HGDIOBJ oldPen = SelectObject(dc, pen);
  RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
  SelectObject(dc, oldBrush);
  SelectObject(dc, oldPen);
  DeleteObject(brush);
  DeleteObject(pen);
}

void drawTextCentered(HDC dc, const RECT& r, const std::wstring& text, HFONT font, COLORREF color, UINT flags = DT_CENTER | DT_VCENTER | DT_SINGLELINE) {
  HGDIOBJ old = SelectObject(dc, font);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, color);
  RECT copy = r;
  DrawTextW(dc, text.c_str(), -1, &copy, flags);
  SelectObject(dc, old);
}

void drawTextCenteredA(HDC dc, const RECT& r, const std::string& text, HFONT font, COLORREF color, UINT flags = DT_CENTER | DT_VCENTER | DT_SINGLELINE) {
  HGDIOBJ old = SelectObject(dc, font);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, color);
  RECT copy = r;
  DrawTextA(dc, text.c_str(), -1, &copy, flags);
  SelectObject(dc, old);
}

XFORM makeRotateXform(double angle, double cx, double cy) {
  XFORM xf{};
  xf.eM11 = static_cast<FLOAT>(std::cos(angle));
  xf.eM12 = static_cast<FLOAT>(-std::sin(angle));
  xf.eM21 = static_cast<FLOAT>(std::sin(angle));
  xf.eM22 = static_cast<FLOAT>(std::cos(angle));
  xf.eDx = static_cast<FLOAT>(cx);
  xf.eDy = static_cast<FLOAT>(cy);
  return xf;
}

void resetWorldTransform(HDC dc) {
  XFORM id{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
  SetWorldTransform(dc, &id);
}

// ---------- Tile rendering ----------

namespace tileColors {
const COLORREF kBlack = RGB(20, 20, 20);
const COLORREF kRed = RGB(190, 28, 36);
const COLORREF kGreen = RGB(36, 130, 60);
const COLORREF kBlue = RGB(28, 60, 150);
const COLORREF kDarkGreen = RGB(20, 90, 44);
}

void drawRingLocal(HDC dc, double cx, double cy, double r, COLORREF color, double thickness) {
  HPEN pen = CreatePen(PS_SOLID, std::max(1, (int)thickness), color);
  HGDIOBJ oldPen = SelectObject(dc, pen);
  HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
  Ellipse(dc, (int)(cx - r), (int)(cy - r), (int)(cx + r), (int)(cy + r));
  SelectObject(dc, oldPen);
  SelectObject(dc, oldBrush);
  DeleteObject(pen);
  // Inner small filled dot
  HBRUSH dotBrush = CreateSolidBrush(color);
  HGDIOBJ ob = SelectObject(dc, dotBrush);
  HPEN np = (HPEN)GetStockObject(NULL_PEN);
  HGDIOBJ op = SelectObject(dc, np);
  double ir = r * 0.35;
  Ellipse(dc, (int)(cx - ir), (int)(cy - ir), (int)(cx + ir), (int)(cy + ir));
  SelectObject(dc, ob);
  SelectObject(dc, op);
  DeleteObject(dotBrush);
}

void drawBambooStickLocal(HDC dc, double cx, double cy, double w, double h, COLORREF color) {
  HBRUSH br = CreateSolidBrush(color);
  HGDIOBJ ob = SelectObject(dc, br);
  HPEN pen = CreatePen(PS_SOLID, 1, RGB(30, 60, 30));
  HGDIOBJ op = SelectObject(dc, pen);
  RoundRect(dc, (int)(cx - w / 2), (int)(cy - h / 2), (int)(cx + w / 2), (int)(cy + h / 2),
            (int)std::max(2.0, w * 0.6), (int)std::max(2.0, w * 0.6));
  // Add a thin lighter stripe down the middle for "bamboo joint" feel
  HPEN linePen = CreatePen(PS_SOLID, 1, RGB(220, 240, 220));
  SelectObject(dc, linePen);
  MoveToEx(dc, (int)cx, (int)(cy - h / 2 + 2), nullptr);
  LineTo(dc, (int)cx, (int)(cy + h / 2 - 2));
  SelectObject(dc, op);
  SelectObject(dc, ob);
  DeleteObject(pen);
  DeleteObject(linePen);
  DeleteObject(br);
}

void drawSmallIndexLocal(HDC dc, int w, int h, const std::wstring& text, COLORREF color) {
  if (text.empty()) return;
  RECT r{-w / 2 + 2, -h / 2 + 1, -w / 2 + std::max(10, w / 3), -h / 2 + std::max(10, h / 4)};
  drawTextCentered(dc, r, text, g_app.tileIndexFont, color, DT_LEFT | DT_TOP | DT_SINGLELINE);
}

// Dot positions per rank (normalized -1..+1 inside the dot zone)
struct DotPos { double x, y; int colorIdx; };
std::vector<DotPos> dotPositionsForRank(int rank) {
  using P = DotPos;
  // colorIdx: 0=blue, 1=red, 2=green
  switch (rank) {
    case 1: return {{0.0, 0.0, 1}};
    case 2: return {{0.0, -0.55, 0}, {0.0, 0.55, 0}};
    case 3: return {{-0.55, -0.55, 0}, {0.0, 0.0, 1}, {0.55, 0.55, 0}};
    case 4: return {{-0.55, -0.55, 0}, {0.55, -0.55, 2}, {-0.55, 0.55, 2}, {0.55, 0.55, 0}};
    case 5: return {{-0.55, -0.55, 0}, {0.55, -0.55, 2}, {0.0, 0.0, 1},
                    {-0.55, 0.55, 2}, {0.55, 0.55, 0}};
    case 6: return {{-0.55, -0.6, 2}, {-0.55, 0.0, 2}, {-0.55, 0.6, 2},
                    {0.55, -0.6, 1}, {0.55, 0.0, 1}, {0.55, 0.6, 1}};
    case 7: return {{0.0, -0.7, 1}, {-0.55, -0.1, 0}, {0.0, -0.1, 0}, {0.55, -0.1, 0},
                    {-0.55, 0.5, 0}, {0.0, 0.5, 0}, {0.55, 0.5, 0}};
    case 8: return {{-0.55, -0.65, 0}, {0.55, -0.65, 0},
                    {-0.55, -0.2, 0}, {0.55, -0.2, 0},
                    {-0.55, 0.25, 0}, {0.55, 0.25, 0},
                    {-0.55, 0.7, 0}, {0.55, 0.7, 0}};
    case 9: return {{-0.6, -0.6, 0}, {0.0, -0.6, 2}, {0.6, -0.6, 0},
                    {-0.6, 0.0, 2}, {0.0, 0.0, 1}, {0.6, 0.0, 2},
                    {-0.6, 0.6, 0}, {0.0, 0.6, 2}, {0.6, 0.6, 0}};
  }
  return {};
}

COLORREF dotColor(int idx) {
  if (idx == 1) return tileColors::kRed;
  if (idx == 2) return tileColors::kGreen;
  return tileColors::kBlue;
}

void drawDotsPatternLocal(HDC dc, int w, int h, int rank) {
  // Reserve top-left for small index. Dot zone occupies rest.
  const double zoneL = -w / 2.0 + std::max(4.0, w * 0.10);
  const double zoneR = w / 2.0 - std::max(3.0, w * 0.06);
  const double zoneT = -h / 2.0 + std::max(8.0, h * 0.15);
  const double zoneB = h / 2.0 - std::max(3.0, h * 0.06);
  const double zoneCx = (zoneL + zoneR) / 2.0;
  const double zoneCy = (zoneT + zoneB) / 2.0;
  const double zoneHW = (zoneR - zoneL) / 2.0;
  const double zoneHH = (zoneB - zoneT) / 2.0;
  double dotRadius = std::max(2.5, std::min(zoneHW, zoneHH) * 0.34);
  if (rank >= 6) dotRadius *= 0.78;
  if (rank == 1) dotRadius = std::min(zoneHW, zoneHH) * 0.55;

  for (const auto& p : dotPositionsForRank(rank)) {
    double x = zoneCx + p.x * zoneHW * 0.6;
    double y = zoneCy + p.y * zoneHH * 0.6;
    drawRingLocal(dc, x, y, dotRadius, dotColor(p.colorIdx), std::max(1.0, dotRadius * 0.25));
  }
}

void drawBambooBird(HDC dc, double cx, double cy, double size) {
  // Simple bird: red body + green wing
  HBRUSH body = CreateSolidBrush(tileColors::kRed);
  HGDIOBJ ob = SelectObject(dc, body);
  HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 20, 20));
  HGDIOBJ op = SelectObject(dc, pen);
  Ellipse(dc, (int)(cx - size * 0.55), (int)(cy - size * 0.35),
          (int)(cx + size * 0.55), (int)(cy + size * 0.4));
  // Beak triangle
  POINT beak[3] = {{(LONG)(cx + size * 0.5), (LONG)(cy - size * 0.1)},
                   {(LONG)(cx + size * 0.9), (LONG)cy},
                   {(LONG)(cx + size * 0.5), (LONG)(cy + size * 0.1)}};
  HBRUSH yellow = CreateSolidBrush(RGB(230, 180, 30));
  SelectObject(dc, yellow);
  Polygon(dc, beak, 3);
  // Wing (green)
  HBRUSH wing = CreateSolidBrush(tileColors::kGreen);
  SelectObject(dc, wing);
  POINT wpts[4] = {{(LONG)(cx - size * 0.3), (LONG)(cy - size * 0.2)},
                   {(LONG)(cx + size * 0.2), (LONG)(cy - size * 0.4)},
                   {(LONG)(cx + size * 0.2), (LONG)(cy + size * 0.1)},
                   {(LONG)(cx - size * 0.3), (LONG)(cy + size * 0.15)}};
  Polygon(dc, wpts, 4);
  // Eye
  HBRUSH eye = CreateSolidBrush(RGB(0, 0, 0));
  SelectObject(dc, eye);
  Ellipse(dc, (int)(cx + size * 0.32), (int)(cy - size * 0.15),
          (int)(cx + size * 0.45), (int)(cy - size * 0.02));
  SelectObject(dc, op);
  SelectObject(dc, ob);
  DeleteObject(pen);
  DeleteObject(body);
  DeleteObject(yellow);
  DeleteObject(wing);
  DeleteObject(eye);
}

void drawBambooPatternLocal(HDC dc, int w, int h, int rank) {
  const double zoneL = -w / 2.0 + std::max(4.0, w * 0.10);
  const double zoneR = w / 2.0 - std::max(3.0, w * 0.06);
  const double zoneT = -h / 2.0 + std::max(8.0, h * 0.15);
  const double zoneB = h / 2.0 - std::max(3.0, h * 0.06);
  const double zoneCx = (zoneL + zoneR) / 2.0;
  const double zoneCy = (zoneT + zoneB) / 2.0;
  const double zoneW = zoneR - zoneL;
  const double zoneH = zoneB - zoneT;

  if (rank == 1) {
    drawBambooBird(dc, zoneCx, zoneCy, std::min(zoneW, zoneH) * 0.85);
    return;
  }

  auto stickHeight = [&](int rowCount) { return zoneH / (rowCount * 1.05); };
  auto stickWidth = [&](int colCount) { return std::min(zoneW / (colCount * 1.4), 7.0); };

  // Layout per rank: columns x rows + optional special top tile
  // We'll position sticks in a grid.
  struct StickGrid { int rows; int cols; bool topSpecial; int topColorIdx; std::vector<std::pair<int,int>> redCells; };
  auto layoutFor = [](int r) -> StickGrid {
    switch (r) {
      case 2: return {2, 1, false, 0, {}};                          // 2 vertical green
      case 3: return {3, 1, false, 0, {{0, 0}}};                    // top red, others green (single column)
      case 4: return {2, 2, false, 0, {}};                          // 2x2 green
      case 5: return {3, 2, false, 0, {{1, 0}}};                    // 5 = 2x2 + center
      case 6: return {3, 2, false, 0, {{0, 0}, {1, 0}, {2, 0}}};    // 2 cols of 3, one col red
      case 7: return {3, 2, true, 1, {}};                           // top red + 2x3 green
      case 8: return {4, 2, false, 0, {}};                          // 2 cols of 4
      case 9: return {3, 3, false, 0, {{0, 1}, {1, 1}, {2, 1}}};    // 3x3 with middle column red
    }
    return {1, 1, false, 0, {}};
  };

  // Handle 5 specially (cross pattern)
  if (rank == 5) {
    double sw = stickWidth(2);
    double sh = stickHeight(2) * 0.9;
    double dx = zoneW * 0.28;
    double dy = zoneH * 0.28;
    drawBambooStickLocal(dc, zoneCx - dx, zoneCy - dy, sw, sh, tileColors::kGreen);
    drawBambooStickLocal(dc, zoneCx + dx, zoneCy - dy, sw, sh, tileColors::kGreen);
    drawBambooStickLocal(dc, zoneCx, zoneCy, sw, sh, tileColors::kRed);
    drawBambooStickLocal(dc, zoneCx - dx, zoneCy + dy, sw, sh, tileColors::kGreen);
    drawBambooStickLocal(dc, zoneCx + dx, zoneCy + dy, sw, sh, tileColors::kGreen);
    return;
  }

  StickGrid g = layoutFor(rank);
  double sw = stickWidth(g.cols);
  double effectiveRows = g.rows + (g.topSpecial ? 1 : 0);
  double sh = stickHeight((int)effectiveRows) * 0.92;
  double rowGap = zoneH / (effectiveRows);
  double colGap = zoneW / std::max(1, g.cols);

  if (g.topSpecial) {
    drawBambooStickLocal(dc, zoneCx, zoneT + rowGap * 0.5, sw, sh,
                         g.topColorIdx == 1 ? tileColors::kRed : tileColors::kGreen);
  }

  auto isRedCell = [&](int row, int col) {
    for (auto& c : g.redCells) if (c.first == row && c.second == col) return true;
    return false;
  };

  double yStart = g.topSpecial ? (zoneT + rowGap * 1.5) : (zoneT + rowGap * 0.5);
  for (int row = 0; row < g.rows; ++row) {
    for (int col = 0; col < g.cols; ++col) {
      double x = zoneL + colGap * (col + 0.5);
      double y = yStart + row * rowGap;
      COLORREF c = isRedCell(row, col) ? tileColors::kRed : tileColors::kGreen;
      drawBambooStickLocal(dc, x, y, sw, sh, c);
    }
  }
}

std::wstring chineseNumeral(int rank) {
  static const wchar_t* nums[] = {L"\u3007", L"\u4E00", L"\u4E8C", L"\u4E09", L"\u56DB",
                                   L"\u4F0D", L"\u516D", L"\u4E03", L"\u516B", L"\u4E5D"};
  if (rank >= 1 && rank <= 9) return nums[rank];
  return L"?";
}

std::wstring tileIndexText(const mahjong::Tile& tile) {
  if (tile.category == mahjong::Category::Suit && tile.rank) {
    return std::wstring(1, static_cast<wchar_t>(L'0' + *tile.rank));
  }
  if (tile.category == mahjong::Category::Wind) {
    switch (*tile.wind) {
      case mahjong::Wind::East: return L"E";
      case mahjong::Wind::South: return L"S";
      case mahjong::Wind::West: return L"W";
      case mahjong::Wind::North: return L"N";
    }
  }
  if (tile.category == mahjong::Category::Dragon) {
    if (*tile.dragon == mahjong::Dragon::Red) return L"C";
    if (*tile.dragon == mahjong::Dragon::Green) return L"F";
    return L"";
  }
  if (tile.category == mahjong::Category::Flower) {
    if (tile.key == "flower-plum") return L"1";
    if (tile.key == "flower-orchid") return L"2";
    if (tile.key == "flower-chrysanthemum") return L"3";
    return L"4";
  }
  if (tile.category == mahjong::Category::Season) {
    if (tile.key == "season-spring") return L"1";
    if (tile.key == "season-summer") return L"2";
    if (tile.key == "season-autumn") return L"3";
    return L"4";
  }
  return L"";
}

// Draw a tile centered at the world-transform origin. width/height in local coords.
// highlightMode: 0 = none, 1 = hover, 2 = just drawn
void drawTileFaceLocal(HDC dc, int w, int h, const mahjong::Tile& tile, int highlightMode = 0) {
  const RECT body{-w / 2, -h / 2, w / 2, h / 2};
  // Shadow
  RECT shadow = body;
  OffsetRect(&shadow, 2, 3);
  HBRUSH shadowBrush = CreateSolidBrush(RGB(0, 0, 0));
  HGDIOBJ oldBrush = SelectObject(dc, shadowBrush);
  HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
  HGDIOBJ oldPen = SelectObject(dc, nullPen);
  RoundRect(dc, shadow.left, shadow.top, shadow.right, shadow.bottom, 8, 8);
  SelectObject(dc, oldBrush);
  SelectObject(dc, oldPen);
  DeleteObject(shadowBrush);

  COLORREF face = RGB(250, 246, 230);
  COLORREF rim = RGB(120, 110, 90);
  int rimWidth = 1;
  if (highlightMode == 1) {        // hover
    face = RGB(255, 246, 200);
    rim = RGB(180, 140, 40);
    rimWidth = 2;
  } else if (highlightMode == 2) { // just drawn
    face = RGB(255, 234, 170);
    rim = RGB(218, 152, 30);
    rimWidth = 2;
  }
  fillRoundRect(dc, body, 8, face, rim, rimWidth);

  // Top-left index
  std::wstring indexText = tileIndexText(tile);
  if (!indexText.empty()) {
    drawSmallIndexLocal(dc, w, h, indexText, tileColors::kRed);
  }

  // Body content per tile type
  if (tile.category == mahjong::Category::Suit && tile.suit && tile.rank) {
    const int rank = *tile.rank;
    if (*tile.suit == mahjong::Suit::Dots) {
      drawDotsPatternLocal(dc, w, h, rank);
    } else if (*tile.suit == mahjong::Suit::Bamboo) {
      drawBambooPatternLocal(dc, w, h, rank);
    } else {
      // Characters (萬): Chinese numeral on top, 萬 on bottom
      COLORREF topCol = (rank == 5) ? tileColors::kRed : tileColors::kBlue;
      RECT topR{-w / 2 + 2, -h / 2 + std::max(8, h / 6), w / 2 - 2, h / 10};
      drawTextCentered(dc, topR, chineseNumeral(rank),
                       (w >= 36) ? g_app.tileBigFont : g_app.tileSmallFont, topCol);
      RECT botR{-w / 2 + 2, h / 10, w / 2 - 2, h / 2 - 2};
      drawTextCentered(dc, botR, L"\u842C",  // 萬
                       (w >= 36) ? g_app.tileBigFont : g_app.tileSmallFont, tileColors::kRed);
    }
    return;
  }

  if (tile.category == mahjong::Category::Wind) {
    std::wstring kanji = L"\u6771";
    COLORREF col = tileColors::kBlue;
    switch (*tile.wind) {
      case mahjong::Wind::East: kanji = L"\u6771"; col = tileColors::kRed; break;
      case mahjong::Wind::South: kanji = L"\u5357"; break;
      case mahjong::Wind::West: kanji = L"\u897F"; break;
      case mahjong::Wind::North: kanji = L"\u5317"; break;
    }
    RECT centerR{-w / 2 + 2, -h / 2 + std::max(8, h / 7), w / 2 - 2, h / 2 - 2};
    drawTextCentered(dc, centerR, kanji,
                     (w >= 36) ? g_app.tileBigFont : g_app.tileSmallFont, col);
    return;
  }

  if (tile.category == mahjong::Category::Dragon) {
    if (*tile.dragon == mahjong::Dragon::White) {
      // Draw blue rectangle outline like real mahjong tiles
      double mx = w * 0.18;
      double my = h * 0.18;
      RECT r{(int)(-w / 2.0 + mx), (int)(-h / 2.0 + my),
             (int)(w / 2.0 - mx), (int)(h / 2.0 - my)};
      HPEN pen = CreatePen(PS_SOLID, std::max(2, w / 18), tileColors::kBlue);
      HGDIOBJ op = SelectObject(dc, pen);
      HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
      Rectangle(dc, r.left, r.top, r.right, r.bottom);
      // Inner cross-corner accents
      MoveToEx(dc, r.left, r.top + (r.bottom - r.top) / 3, nullptr);
      LineTo(dc, r.left + (r.right - r.left) / 3, r.top);
      MoveToEx(dc, r.right, r.bottom - (r.bottom - r.top) / 3, nullptr);
      LineTo(dc, r.right - (r.right - r.left) / 3, r.bottom);
      SelectObject(dc, op);
      SelectObject(dc, ob);
      DeleteObject(pen);
      return;
    }
    std::wstring kanji = (*tile.dragon == mahjong::Dragon::Red) ? L"\u4E2D" : L"\u767C";
    COLORREF col = (*tile.dragon == mahjong::Dragon::Red) ? tileColors::kRed : tileColors::kGreen;
    RECT centerR{-w / 2 + 2, -h / 2 + std::max(8, h / 7), w / 2 - 2, h / 2 - 2};
    drawTextCentered(dc, centerR, kanji,
                     (w >= 36) ? g_app.tileBigFont : g_app.tileSmallFont, col);
    return;
  }

  if (tile.category == mahjong::Category::Flower || tile.category == mahjong::Category::Season) {
    std::wstring kanji = L"\u82B1";  // 花
    COLORREF col = tileColors::kGreen;
    if (tile.category == mahjong::Category::Flower) {
      if (tile.key == "flower-plum") kanji = L"\u6885";
      else if (tile.key == "flower-orchid") kanji = L"\u862D";
      else if (tile.key == "flower-chrysanthemum") kanji = L"\u83CA";
      else kanji = L"\u7AF9";
      col = tileColors::kGreen;
    } else {
      if (tile.key == "season-spring") kanji = L"\u6625";
      else if (tile.key == "season-summer") kanji = L"\u590F";
      else if (tile.key == "season-autumn") kanji = L"\u79CB";
      else kanji = L"\u51AC";
      col = tileColors::kRed;
    }
    RECT centerR{-w / 2 + 2, -h / 2 + std::max(8, h / 6), w / 2 - 2, h / 2 - 2};
    drawTextCentered(dc, centerR, kanji,
                     (w >= 36) ? g_app.tileBigFont : g_app.tileSmallFont, col);
    return;
  }
}

void drawTileBackLocal(HDC dc, int w, int h) {
  const RECT body{-w / 2, -h / 2, w / 2, h / 2};
  // Shadow
  RECT shadow = body;
  OffsetRect(&shadow, 2, 3);
  HBRUSH shadowBrush = CreateSolidBrush(RGB(0, 0, 0));
  HGDIOBJ oldBrush = SelectObject(dc, shadowBrush);
  HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
  HGDIOBJ oldPen = SelectObject(dc, nullPen);
  RoundRect(dc, shadow.left, shadow.top, shadow.right, shadow.bottom, 8, 8);
  SelectObject(dc, oldBrush);
  SelectObject(dc, oldPen);
  DeleteObject(shadowBrush);

  fillRoundRect(dc, body, 8, RGB(68, 132, 96), RGB(30, 70, 50), 1);
  RECT inset = body;
  InflateRect(&inset, -3, -3);
  fillRoundRect(dc, inset, 6, RGB(80, 152, 108), RGB(50, 110, 78), 1);
  // Decorative inner ring
  RECT inner = body;
  InflateRect(&inner, -7, -7);
  HPEN ringPen = CreatePen(PS_SOLID, 1, RGB(120, 180, 140));
  HGDIOBJ oldPen2 = SelectObject(dc, ringPen);
  HGDIOBJ oldBrush2 = SelectObject(dc, GetStockObject(NULL_BRUSH));
  RoundRect(dc, inner.left, inner.top, inner.right, inner.bottom, 4, 4);
  SelectObject(dc, oldPen2);
  SelectObject(dc, oldBrush2);
  DeleteObject(ringPen);
}

// Draws a tile at center (cx, cy), rotated by `angle` radians.
void drawTileAt(HDC dc, double cx, double cy, int w, int h, double angle, const mahjong::Tile& tile, int highlightMode = 0) {
  SaveDC(dc);
  SetGraphicsMode(dc, GM_ADVANCED);
  XFORM xf = makeRotateXform(angle, cx, cy);
  SetWorldTransform(dc, &xf);
  drawTileFaceLocal(dc, w, h, tile, highlightMode);
  RestoreDC(dc, -1);
}

void drawTileBackAt(HDC dc, double cx, double cy, int w, int h, double angle) {
  SaveDC(dc);
  SetGraphicsMode(dc, GM_ADVANCED);
  XFORM xf = makeRotateXform(angle, cx, cy);
  SetWorldTransform(dc, &xf);
  drawTileBackLocal(dc, w, h);
  RestoreDC(dc, -1);
}

// ---------- Layout ----------

struct Layout {
  RECT tableRect;
  RECT feltRect;
  POINT center;
};

Layout computeLayout(int clientWidth, int clientHeight) {
  Layout layout{};
  layout.tableRect = {0, kToolbarHeight + kStatusHeight, clientWidth - kActionPanelWidth, clientHeight};
  if (layout.tableRect.right < 600) layout.tableRect.right = 600;
  layout.feltRect = layout.tableRect;
  InflateRect(&layout.feltRect, -18, -18);
  layout.center.x = (layout.feltRect.left + layout.feltRect.right) / 2;
  layout.center.y = (layout.feltRect.top + layout.feltRect.bottom) / 2;
  return layout;
}

int slotForSeat(int seatIndex) {
  return ((seatIndex - g_app.viewerSeat) % 4 + 4) % 4;
}

double angleForSlot(int slot) {
  switch (slot) {
    case 0: return 0.0;
    case 1: return -M_PI / 2.0;
    case 2: return M_PI;
    case 3: return M_PI / 2.0;
  }
  return 0.0;
}

// ---------- Table background ----------

void drawTableBackground(HDC dc, const Layout& layout) {
  // Wood frame
  fillRoundRect(dc, layout.tableRect, 26, RGB(120, 70, 36), RGB(60, 32, 16), 2);
  // Highlight inset
  RECT highlight = layout.tableRect;
  InflateRect(&highlight, -4, -4);
  fillRoundRect(dc, highlight, 22, RGB(150, 92, 48), RGB(95, 55, 28), 1);
  // Green felt
  fillRoundRect(dc, layout.feltRect, 18, RGB(31, 110, 70), RGB(18, 70, 45), 2);
  // Concentric inner ring (vignette feel)
  RECT inner1 = layout.feltRect; InflateRect(&inner1, -28, -28);
  HPEN ring1 = CreatePen(PS_SOLID, 1, RGB(20, 90, 60));
  HGDIOBJ oldPen = SelectObject(dc, ring1);
  HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
  RoundRect(dc, inner1.left, inner1.top, inner1.right, inner1.bottom, 16, 16);
  SelectObject(dc, oldPen); DeleteObject(ring1);
  // Faint diagonal lines from corners toward center
  HPEN linePen = CreatePen(PS_SOLID, 1, RGB(22, 80, 52));
  SelectObject(dc, linePen);
  MoveToEx(dc, layout.feltRect.left + 80, layout.feltRect.top + 80, nullptr);
  LineTo(dc, layout.center.x - 90, layout.center.y - 90);
  MoveToEx(dc, layout.feltRect.right - 80, layout.feltRect.top + 80, nullptr);
  LineTo(dc, layout.center.x + 90, layout.center.y - 90);
  MoveToEx(dc, layout.feltRect.left + 80, layout.feltRect.bottom - 80, nullptr);
  LineTo(dc, layout.center.x - 90, layout.center.y + 90);
  MoveToEx(dc, layout.feltRect.right - 80, layout.feltRect.bottom - 80, nullptr);
  LineTo(dc, layout.center.x + 90, layout.center.y + 90);
  SelectObject(dc, oldPen);
  SelectObject(dc, oldBrush);
  DeleteObject(linePen);
}

// ---------- Discard wells ----------

RECT discardWellRectForSlot(const Layout& layout, int slot) {
  const int cx = layout.center.x;
  const int cy = layout.center.y;
  // Each well sized to hold 6x4 grid of 28x38 tiles plus padding
  const int W = 200;  // long axis
  const int T = 160;  // short axis
  switch (slot) {
    case 0: { // bottom — wide and short
      return RECT{cx - W / 2, cy + 95, cx + W / 2, cy + 95 + T};
    }
    case 2: { // top
      return RECT{cx - W / 2, cy - 95 - T, cx + W / 2, cy - 95};
    }
    case 1: { // right
      return RECT{cx + 95, cy - W / 2, cx + 95 + T, cy + W / 2};
    }
    case 3: { // left
      return RECT{cx - 95 - T, cy - W / 2, cx - 95, cy + W / 2};
    }
  }
  return RECT{0, 0, 0, 0};
}

void drawDiscardWells(HDC dc, const Layout& layout) {
  for (int slot = 0; slot < 4; ++slot) {
    RECT r = discardWellRectForSlot(layout, slot);
    fillRoundRect(dc, r, 10, RGB(22, 88, 58), RGB(80, 130, 96), 1);
    RECT inset = r; InflateRect(&inset, -3, -3);
    fillRoundRect(dc, inset, 8, RGB(26, 96, 64), RGB(50, 110, 78), 1);
  }
}

// ---------- Center info plaque ----------

std::string formatRoundLabel(const mahjong::RoundState& state) {
  std::string wind;
  switch (state.prevailingWind) {
    case mahjong::Wind::East: wind = "East"; break;
    case mahjong::Wind::South: wind = "South"; break;
    case mahjong::Wind::West: wind = "West"; break;
    case mahjong::Wind::North: wind = "North"; break;
  }
  return wind + " round";
}

void drawCenterPlaque(HDC dc, const Layout& layout) {
  const int size = 180;
  RECT plaque{layout.center.x - size / 2, layout.center.y - size / 2,
              layout.center.x + size / 2, layout.center.y + size / 2};
  // Outer gold ring
  fillRoundRect(dc, plaque, 18, RGB(58, 42, 18), RGB(232, 196, 96), 3);
  // Inner felt
  RECT mid = plaque; InflateRect(&mid, -6, -6);
  fillRoundRect(dc, mid, 14, RGB(24, 64, 44), RGB(160, 120, 60), 1);
  RECT inner = mid; InflateRect(&inner, -4, -4);
  fillRoundRect(dc, inner, 10, RGB(18, 50, 36), RGB(60, 90, 70), 1);

  if (!g_app.hasRound) {
    drawTextCenteredA(dc, plaque, "Hong Kong\nMahjong", g_app.plaqueLargeFont, RGB(232, 220, 170), DT_CENTER | DT_VCENTER | DT_WORDBREAK);
    return;
  }

  const auto& state = g_app.round;

  // Round label at top
  RECT title{plaque.left, plaque.top + 8, plaque.right, plaque.top + 28};
  drawTextCenteredA(dc, title, formatRoundLabel(state), g_app.plaqueFont, RGB(232, 200, 130));

  // Big prevailing wind kanji centered
  std::wstring windKanji = L"\u6771";
  switch (state.prevailingWind) {
    case mahjong::Wind::East: windKanji = L"\u6771"; break;
    case mahjong::Wind::South: windKanji = L"\u5357"; break;
    case mahjong::Wind::West: windKanji = L"\u897F"; break;
    case mahjong::Wind::North: windKanji = L"\u5317"; break;
  }
  RECT windRect{plaque.left + 30, plaque.top + 30, plaque.right - 30, plaque.bottom - 60};
  drawTextCentered(dc, windRect, windKanji, g_app.centerLargeFont, RGB(240, 210, 130));

  // Bottom info: Wall + Turn
  char line1[64];
  std::snprintf(line1, sizeof(line1), "Wall %d   Turn %d",
                static_cast<int>(state.wall.liveWall.size()), state.turnNumber);
  RECT line1Rect{plaque.left + 6, plaque.bottom - 56, plaque.right - 6, plaque.bottom - 36};
  drawTextCenteredA(dc, line1Rect, line1, g_app.plaqueFont, RGB(232, 220, 170));

  char line2[96];
  std::snprintf(line2, sizeof(line2), "%s   Min Fan %d   Dealer %d",
                state.phase == mahjong::Phase::AwaitingDraw ? "draw"
                : state.phase == mahjong::Phase::AwaitingDiscard ? "discard"
                : state.phase == mahjong::Phase::AwaitingClaims ? "claims"
                : "finished",
                state.rules.minFan, state.dealerSeat);
  RECT line2Rect{plaque.left + 6, plaque.bottom - 32, plaque.right - 6, plaque.bottom - 10};
  drawTextCenteredA(dc, line2Rect, line2, g_app.plaqueFont, RGB(210, 200, 160));
}

// ---------- Player plaques ----------

std::string seatWindString(mahjong::Wind wind) {
  switch (wind) {
    case mahjong::Wind::East: return "East";
    case mahjong::Wind::South: return "South";
    case mahjong::Wind::West: return "West";
    case mahjong::Wind::North: return "North";
  }
  return "?";
}

std::wstring seatWindKanji(mahjong::Wind wind) {
  switch (wind) {
    case mahjong::Wind::East: return L"\u6771";
    case mahjong::Wind::South: return L"\u5357";
    case mahjong::Wind::West: return L"\u897F";
    case mahjong::Wind::North: return L"\u5317";
  }
  return L"?";
}

RECT plaqueRectForSlot(const Layout& layout, int slot) {
  const int hSize = 200;
  const int vThickness = 44;
  const int vSize = 180;
  const int hThickness = 44;
  const int cx = layout.center.x;
  const int cy = layout.center.y;
  switch (slot) {
    case 0: { // bottom: between hand and discards
      RECT r{cx - hSize / 2, layout.feltRect.bottom - 132,
             cx + hSize / 2, layout.feltRect.bottom - 132 + hThickness};
      return r;
    }
    case 2: { // top
      RECT r{cx - hSize / 2, layout.feltRect.top + 88,
             cx + hSize / 2, layout.feltRect.top + 88 + hThickness};
      return r;
    }
    case 1: { // right
      RECT r{layout.feltRect.right - 132, cy - vSize / 2,
             layout.feltRect.right - 132 + vThickness, cy + vSize / 2};
      return r;
    }
    case 3: { // left
      RECT r{layout.feltRect.left + 88, cy - vSize / 2,
             layout.feltRect.left + 88 + vThickness, cy + vSize / 2};
      return r;
    }
  }
  return RECT{0, 0, 0, 0};
}

void drawPlayerPlaque(HDC dc, const Layout& layout, const mahjong::PlayerState& player) {
  const int slot = slotForSeat(player.seatIndex);
  RECT r = plaqueRectForSlot(layout, slot);
  const bool isCurrent = g_app.hasRound && g_app.round.currentTurn == player.seatIndex && g_app.round.phase != mahjong::Phase::Finished;
  const bool isDealer = g_app.hasRound && g_app.round.dealerSeat == player.seatIndex;
  const COLORREF border = isCurrent ? RGB(255, 200, 88) : RGB(120, 100, 60);
  const COLORREF fill = isCurrent ? RGB(64, 92, 64) : RGB(30, 48, 36);
  fillRoundRect(dc, r, 10, fill, border, isCurrent ? 2 : 1);

  // Badge size
  const int badge = 32;
  const int rectW = r.right - r.left;
  const int rectH = r.bottom - r.top;
  RECT badgeRect;
  RECT textRect;
  const bool horizontal = (slot == 0 || slot == 2);
  if (slot == 0) {
    // badge on left, text on right
    badgeRect = {r.left + 6, r.top + (rectH - badge) / 2, r.left + 6 + badge, r.top + (rectH - badge) / 2 + badge};
    textRect = {badgeRect.right + 6, r.top + 4, r.right - 6, r.bottom - 4};
  } else if (slot == 2) {
    badgeRect = {r.right - 6 - badge, r.top + (rectH - badge) / 2, r.right - 6, r.top + (rectH - badge) / 2 + badge};
    textRect = {r.left + 6, r.top + 4, badgeRect.left - 6, r.bottom - 4};
  } else if (slot == 1) {
    badgeRect = {r.left + (rectW - badge) / 2, r.top + 6, r.left + (rectW - badge) / 2 + badge, r.top + 6 + badge};
    textRect = {r.left + 2, badgeRect.bottom + 4, r.right - 2, r.bottom - 6};
  } else {
    badgeRect = {r.left + (rectW - badge) / 2, r.bottom - 6 - badge, r.left + (rectW - badge) / 2 + badge, r.bottom - 6};
    textRect = {r.left + 2, r.top + 6, r.right - 2, badgeRect.top - 4};
  }

  COLORREF windBg = (player.wind == mahjong::Wind::East) ? RGB(192, 56, 56) : RGB(40, 64, 96);
  fillRoundRect(dc, badgeRect, 6, windBg, RGB(220, 200, 140), 1);
  drawTextCentered(dc, badgeRect, seatWindKanji(player.wind), g_app.plaqueLargeFont, RGB(245, 235, 200));

  char nameLine[160];
  const char* role = (player.controller == mahjong::Controller::Human) ? "you" : "AI";
  const char* dealerMark = isDealer ? " *" : "";
  if (horizontal) {
    std::snprintf(nameLine, sizeof(nameLine), "%s (%s)\n%d pts%s", player.displayName.c_str(), role, player.score, dealerMark);
    drawTextCenteredA(dc, textRect, nameLine, g_app.plaqueFont, RGB(232, 222, 188), DT_LEFT | DT_VCENTER | DT_WORDBREAK);
  } else {
    std::snprintf(nameLine, sizeof(nameLine), "%s\n(%s)\n%d pts%s", player.displayName.c_str(), role, player.score, dealerMark);
    drawTextCenteredA(dc, textRect, nameLine, g_app.plaqueFont, RGB(232, 222, 188), DT_CENTER | DT_VCENTER | DT_WORDBREAK);
  }
}

// ---------- Hand / meld / discard rendering ----------

struct SeatGeometry {
  // Hand
  double handCenterX;
  double handCenterY;
  // Discard zone center
  double discardCenterX;
  double discardCenterY;
  // Meld zone start corner
  double meldStartX;
  double meldStartY;
};

SeatGeometry computeSeatGeometry(const Layout& layout, int slot) {
  SeatGeometry g{};
  const int cx = layout.center.x;
  const int cy = layout.center.y;
  // Melds are placed inset further from the felt edge than the hand so they
  // never visually overlap the player's tiles.  The hand sits at edge-50 and is
  // 58px tall, so its near boundary is at edge-79.  Anchoring melds at edge-115
  // (40px tall) leaves them entirely above/outside the hand row.
  switch (slot) {
    case 0: { // bottom
      g.handCenterX = cx;
      g.handCenterY = layout.feltRect.bottom - 50;
      g.discardCenterX = cx;
      g.discardCenterY = cy + 170;
      g.meldStartX = cx + 330;
      g.meldStartY = layout.feltRect.bottom - 115;
      break;
    }
    case 2: { // top
      g.handCenterX = cx;
      g.handCenterY = layout.feltRect.top + 40;
      g.discardCenterX = cx;
      g.discardCenterY = cy - 170;
      g.meldStartX = cx - 330;
      g.meldStartY = layout.feltRect.top + 115;
      break;
    }
    case 1: { // right
      g.handCenterX = layout.feltRect.right - 50;
      g.handCenterY = cy;
      g.discardCenterX = cx + 170;
      g.discardCenterY = cy;
      g.meldStartX = layout.feltRect.right - 115;
      g.meldStartY = cy + 220;
      break;
    }
    case 3: { // left
      g.handCenterX = layout.feltRect.left + 40;
      g.handCenterY = cy;
      g.discardCenterX = cx - 170;
      g.discardCenterY = cy;
      g.meldStartX = layout.feltRect.left + 115;
      g.meldStartY = cy - 220;
      break;
    }
  }
  return g;
}

void drawHand(HDC dc, const Layout& layout, const mahjong::PlayerState& player, bool faceUp) {
  const int slot = slotForSeat(player.seatIndex);
  const SeatGeometry geom = computeSeatGeometry(layout, slot);
  if (player.concealedTiles.empty()) return;

  // Determine if there's a freshly drawn tile to display separately (kept at the right end).
  std::optional<std::string> drawnId;
  if (g_app.hasRound && g_app.round.lastDraw &&
      g_app.round.lastDraw->seatIndex == player.seatIndex &&
      g_app.round.phase == mahjong::Phase::AwaitingDiscard &&
      g_app.round.currentTurn == player.seatIndex) {
    const std::string& did = g_app.round.lastDraw->tile.id;
    for (const auto& t : player.concealedTiles) {
      if (t.id == did) { drawnId = did; break; }
    }
  }

  // Build display order: sorted tiles minus drawn, plus drawn appended.
  std::vector<mahjong::Tile> rest;
  rest.reserve(player.concealedTiles.size());
  std::optional<mahjong::Tile> drawnTile;
  for (const auto& t : player.concealedTiles) {
    if (drawnId && t.id == *drawnId && !drawnTile) {
      drawnTile = t;
    } else {
      rest.push_back(t);
    }
  }
  rest = mahjong::sortTiles(rest);
  std::vector<mahjong::Tile> tiles = rest;
  if (drawnTile) tiles.push_back(*drawnTile);

  const int n = static_cast<int>(tiles.size());
  if (n == 0) return;

  const int handTileW = (slot == 0) ? 42 : 26;
  const int handTileH = (slot == 0) ? 58 : 36;
  const int spacing = 2;
  const int extraGap = 10;  // separation between sorted hand and freshly drawn tile
  const int gapCount = drawnTile ? 1 : 0;
  const int totalLength = n * handTileW + (n - 1) * spacing + gapCount * extraGap;
  const double angle = angleForSlot(slot);

  const bool horizontal = (slot == 0 || slot == 2);
  const double startMain = horizontal
                               ? (geom.handCenterX - totalLength / 2.0 + handTileW / 2.0)
                               : (geom.handCenterY - totalLength / 2.0 + handTileW / 2.0);

  if (slot == 0) {
    g_app.handHits.clear();
  }
  const bool isDiscardPrompt = currentlyAwaitingHumanDiscard() && slot == 0;

  for (int i = 0; i < n; ++i) {
    const bool isDrawn = drawnTile && i == n - 1;
    double offset = i * (handTileW + spacing) + (isDrawn ? extraGap : 0);
    double tileMain = startMain + offset;
    double cx, cy;
    if (slot == 0) { cx = tileMain; cy = geom.handCenterY; }
    else if (slot == 2) { cx = tileMain; cy = geom.handCenterY; }
    else if (slot == 1) { cx = geom.handCenterX; cy = tileMain; }
    else { cx = geom.handCenterX; cy = tileMain; }

    int highlightMode = 0;
    if (isDrawn && faceUp) highlightMode = 2;  // gold "just drawn" highlight
    const bool isHover = isDiscardPrompt && tileIsDiscardable(tiles[i].id) &&
                         g_app.hoverTileId.has_value() && *g_app.hoverTileId == tiles[i].id;
    if (isHover) highlightMode = 1;

    if (faceUp) {
      drawTileAt(dc, cx, cy, handTileW, handTileH, angle, tiles[i], highlightMode);
    } else {
      drawTileBackAt(dc, cx, cy, handTileW, handTileH, angle);
    }

    if (slot == 0 && faceUp) {
      RECT hit{static_cast<LONG>(cx - handTileW / 2),
               static_cast<LONG>(cy - handTileH / 2),
               static_cast<LONG>(cx + handTileW / 2),
               static_cast<LONG>(cy + handTileH / 2)};
      g_app.handHits.push_back({hit, tiles[i].id});
    }
  }

  // Flowers next to the hand for the bottom player
  if (!player.flowers.empty()) {
    const auto flowers = mahjong::sortTiles(player.flowers);
    const int fw = (slot == 0) ? 32 : 22;
    const int fh = (slot == 0) ? 44 : 30;
    const int fspacing = 2;
    if (slot == 0) {
      double startX = layout.feltRect.left + 20 + fw / 2.0;
      double y = geom.handCenterY - handTileH / 2.0 - fh / 2.0 - 4;
      for (std::size_t i = 0; i < flowers.size(); ++i) {
        drawTileAt(dc, startX + i * (fw + fspacing), y, fw, fh, angle, flowers[i], 0);
      }
    } else if (slot == 2) {
      double startX = layout.feltRect.right - 20 - fw / 2.0;
      double y = geom.handCenterY + handTileH / 2.0 + fh / 2.0 + 4;
      for (std::size_t i = 0; i < flowers.size(); ++i) {
        drawTileAt(dc, startX - i * (fw + fspacing), y, fw, fh, angle, flowers[i], 0);
      }
    } else if (slot == 1) {
      double x = geom.handCenterX - handTileW / 2.0 - fh / 2.0 - 4;
      double startY = layout.feltRect.top + 20 + fw / 2.0;
      for (std::size_t i = 0; i < flowers.size(); ++i) {
        drawTileAt(dc, x, startY + i * (fw + fspacing), fw, fh, angle, flowers[i], 0);
      }
    } else {
      double x = geom.handCenterX + handTileW / 2.0 + fh / 2.0 + 4;
      double startY = layout.feltRect.bottom - 20 - fw / 2.0;
      for (std::size_t i = 0; i < flowers.size(); ++i) {
        drawTileAt(dc, x, startY - i * (fw + fspacing), fw, fh, angle, flowers[i], 0);
      }
    }
  }
}

void drawMelds(HDC dc, const Layout& layout, const mahjong::PlayerState& player) {
  if (player.melds.empty()) return;
  const int slot = slotForSeat(player.seatIndex);
  const SeatGeometry geom = computeSeatGeometry(layout, slot);
  const double angle = angleForSlot(slot);
  const int tw = 28;
  const int th = 40;
  const int gap = 8;

  // Lay melds along the appropriate axis, each meld a run of tiles separated by gap
  double cursorMain = (slot == 0 || slot == 2) ? geom.meldStartX : geom.meldStartY;
  for (const auto& meld : player.melds) {
    for (std::size_t i = 0; i < meld.tiles.size(); ++i) {
      double cx, cy;
      if (slot == 0) {
        cx = cursorMain + i * (tw + 2);
        cy = geom.meldStartY;
      } else if (slot == 2) {
        cx = cursorMain - i * (tw + 2);
        cy = geom.meldStartY;
      } else if (slot == 1) {
        cx = geom.meldStartX;
        cy = cursorMain + i * (tw + 2);
      } else {
        cx = geom.meldStartX;
        cy = cursorMain - i * (tw + 2);
      }
      drawTileAt(dc, cx, cy, tw, th, angle, meld.tiles[i], 0);
    }
    if (slot == 0) cursorMain += meld.tiles.size() * (tw + 2) + gap;
    else if (slot == 2) cursorMain -= meld.tiles.size() * (tw + 2) + gap;
    else if (slot == 1) cursorMain += meld.tiles.size() * (tw + 2) + gap;
    else cursorMain -= meld.tiles.size() * (tw + 2) + gap;
  }
}

void drawDiscards(HDC dc, const Layout& layout, const mahjong::PlayerState& player) {
  if (player.discards.empty()) return;
  const int slot = slotForSeat(player.seatIndex);
  const SeatGeometry geom = computeSeatGeometry(layout, slot);
  const double angle = angleForSlot(slot);
  const int tw = 28;
  const int th = 38;
  const int gap = 2;
  const int columns = 6;
  const int rows = (static_cast<int>(player.discards.size()) + columns - 1) / columns;
  // Grid extents in tile-local axis: width = columns * tw + (columns-1)*gap, height = rows * th + (rows-1)*gap
  const double gridW = columns * tw + (columns - 1) * gap;
  const double gridH = std::max(1, rows) * th + std::max(0, rows - 1) * gap;
  // Each tile's local position in the rotated frame (local x along row direction, local y along stack direction)
  // We arrange so that local +y grows toward the player (away from center) — visually, more recent discards stack outward
  // To position: compute local (lx, ly), then transform to world using the rotation around (discardCenterX, discardCenterY).

  // Position the top-left of the grid at local (-gridW/2, -gridH/2) relative to discard center.
  const std::optional<std::string> lastDiscardId = g_app.hasRound && g_app.round.lastDiscard ? std::optional<std::string>(g_app.round.lastDiscard->tile.id) : std::nullopt;

  for (std::size_t i = 0; i < player.discards.size(); ++i) {
    const int row = static_cast<int>(i) / columns;
    const int col = static_cast<int>(i) % columns;
    // Local coords centered at the discard zone center.
    const double lx = -gridW / 2.0 + col * (tw + gap) + tw / 2.0;
    const double ly = -gridH / 2.0 + row * (th + gap) + th / 2.0;
    // Apply rotation manually (avoid switching transforms for each tile)
    const double ca = std::cos(angle);
    const double sa = std::sin(angle);
    const double wx = geom.discardCenterX + lx * ca + ly * sa;
    const double wy = geom.discardCenterY + (-lx * sa) + ly * ca;
    const bool highlight = lastDiscardId && *lastDiscardId == player.discards[i].id;
    drawTileAt(dc, wx, wy, tw, th, angle, player.discards[i], highlight ? 1 : 0);
  }
}

// ---------- Conclusion banner ----------

std::string conclusionSourceText(const mahjong::RoundConclusion& conclusion, const mahjong::RoundState& state) {
  if (!conclusion.source) return {};
  auto seatWindAtIndex = [&](int seat) -> std::string {
    if (seat < 0 || seat >= static_cast<int>(state.players.size())) return {};
    return seatWindString(state.players[seat].wind);
  };
  switch (*conclusion.source) {
    case mahjong::WinSource::SelfDraw:
      return "Won by self-draw";
    case mahjong::WinSource::Discard: {
      int by = -1;
      if (conclusion.responsibleSeat) by = *conclusion.responsibleSeat;
      else if (state.lastDiscard) by = state.lastDiscard->bySeat;
      const auto wind = seatWindAtIndex(by);
      return wind.empty() ? std::string("Won on discard") : ("Won on discard from " + wind);
    }
    case mahjong::WinSource::Flower:
      return "Won by flower";
    case mahjong::WinSource::RobbingKong: {
      if (conclusion.responsibleSeat) {
        const auto wind = seatWindAtIndex(*conclusion.responsibleSeat);
        if (!wind.empty()) return "Won by robbing Kong from " + wind;
      }
      return "Won by robbing Kong";
    }
  }
  return {};
}

void drawConclusionBanner(HDC dc, const Layout& layout) {
  if (!g_app.hasRound || !g_app.round.conclusion) return;
  const auto& conclusion = *g_app.round.conclusion;
  const bool hasWinTile = conclusion.winningTile.has_value();
  const int w = 600;
  // Reserve extra vertical space when we have a winning tile to display.
  const int h = hasWinTile ? 290 : 220;
  // Center over the table (overlays center jewel intentionally to draw the eye)
  RECT banner{layout.center.x - w / 2, layout.center.y - h / 2,
              layout.center.x + w / 2, layout.center.y + h / 2};
  // Drop shadow
  RECT shadow = banner; OffsetRect(&shadow, 4, 4);
  fillRoundRect(dc, shadow, 16, RGB(0, 0, 0), RGB(0, 0, 0), 0);
  fillRoundRect(dc, banner, 14, RGB(40, 24, 14), RGB(240, 200, 110), 3);
  RECT inset = banner; InflateRect(&inset, -6, -6);
  fillRoundRect(dc, inset, 10, RGB(56, 36, 18), RGB(160, 120, 60), 1);

  std::string title = conclusion.message;
  drawTextCenteredA(dc, RECT{banner.left + 12, banner.top + 8, banner.right - 12, banner.top + 36}, title, g_app.plaqueLargeFont, RGB(244, 220, 150), DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

  int bodyTop = banner.top + 40;
  if (hasWinTile) {
    const auto srcText = conclusionSourceText(conclusion, g_app.round);
    if (!srcText.empty()) {
      drawTextCenteredA(dc, RECT{banner.left + 12, bodyTop, banner.right - 12, bodyTop + 22}, srcText, g_app.plaqueFont, RGB(255, 233, 168), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    // Render the winning tile, prominent and centered.
    const int tw = 56, th = 76;
    const double tileCx = (banner.left + banner.right) / 2.0;
    const double tileCy = bodyTop + 24 + th / 2.0;
    drawTileAt(dc, tileCx, tileCy, tw, th, 0.0, *conclusion.winningTile, /*highlightMode*/ 1);
    bodyTop = static_cast<int>(tileCy + th / 2.0 + 6);
  }

  if (conclusion.settlement) {
    const auto& s = *conclusion.settlement;
    char info[256];
    std::snprintf(info, sizeof(info), "Fan: %d  (min %d)%s", s.fan, s.minFan, s.eligible ? "" : "  - below minimum, no payment");
    drawTextCenteredA(dc, RECT{banner.left + 12, bodyTop, banner.right - 12, bodyTop + 22}, info, g_app.plaqueFont, RGB(232, 220, 180));

    std::string features;
    for (std::size_t i = 0; i < s.includedFeatures.size(); ++i) {
      if (!features.empty()) features += ", ";
      features += s.includedFeatures[i].name;
    }
    if (features.empty()) features = "(no fan features)";
    drawTextCenteredA(dc, RECT{banner.left + 12, bodyTop + 22, banner.right - 12, bodyTop + 52}, features, g_app.plaqueFont, RGB(220, 210, 180), DT_CENTER | DT_WORDBREAK);

    int y = bodyTop + 54;
    for (const auto& line : s.paymentLines) {
      char pay[128];
      std::snprintf(pay, sizeof(pay), "%s pays %s: %d",
                    seatWindString(line.from).c_str(), seatWindString(line.to).c_str(), line.points);
      drawTextCenteredA(dc, RECT{banner.left + 12, y, banner.right - 12, y + 18}, pay, g_app.plaqueFont, RGB(240, 230, 200), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      y += 18;
      if (y > banner.bottom - 8) break;
    }
  }
}

// ---------- Compose frame ----------

void drawFrame(HDC dc, int width, int height) {
  // Background
  RECT all{0, 0, width, height};
  fillRect(dc, all, RGB(14, 22, 18));

  // Toolbar background
  RECT toolbar{0, 0, width, kToolbarHeight};
  fillRect(dc, toolbar, RGB(28, 38, 32));
  RECT toolbarEdge{0, kToolbarHeight - 2, width, kToolbarHeight};
  fillRect(dc, toolbarEdge, RGB(60, 80, 64));

  // Status bar
  RECT status{0, kToolbarHeight, width, kToolbarHeight + kStatusHeight};
  fillRect(dc, status, RGB(36, 48, 42));

  // Action panel
  RECT actionPanel{width - kActionPanelWidth, kToolbarHeight + kStatusHeight, width, height};
  fillRect(dc, actionPanel, RGB(34, 46, 40));
  RECT actionEdge{actionPanel.left - 2, actionPanel.top, actionPanel.left, actionPanel.bottom};
  fillRect(dc, actionEdge, RGB(60, 80, 64));

  // Action panel header
  RECT actionHeader{actionPanel.left + 12, actionPanel.top + 12, actionPanel.right - 12, actionPanel.top + 40};
  drawTextCenteredA(dc, actionHeader, "Legal actions", g_app.plaqueLargeFont, RGB(238, 220, 150), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

  // Table
  Layout layout = computeLayout(width, height);
  drawTableBackground(dc, layout);
  drawDiscardWells(dc, layout);
  drawCenterPlaque(dc, layout);

  g_app.handHits.clear();

  if (g_app.hasRound) {
    // Draw plaques and contents per seat.
    for (const auto& player : g_app.round.players) {
      const bool isViewer = (player.seatIndex == g_app.viewerSeat);
      const bool showHand = g_app.revealAll || g_app.round.phase == mahjong::Phase::Finished || isViewer;
      drawDiscards(dc, layout, player);
      drawMelds(dc, layout, player);
      drawHand(dc, layout, player, showHand);
      drawPlayerPlaque(dc, layout, player);
    }
    drawConclusionBanner(dc, layout);
  } else {
    drawTextCenteredA(dc, layout.feltRect, "Click \"Play East/South/West/North\" or \"Watch 4 AI\" to begin.",
                      g_app.plaqueLargeFont, RGB(220, 230, 200), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
}

void ensureBackbuffer(HDC referenceDC, int width, int height) {
  if (g_app.backbuffer && g_app.backbufferWidth == width && g_app.backbufferHeight == height) return;
  if (g_app.backbuffer) {
    DeleteObject(g_app.backbuffer);
    g_app.backbuffer = nullptr;
  }
  g_app.backbuffer = CreateCompatibleBitmap(referenceDC, width, height);
  g_app.backbufferWidth = width;
  g_app.backbufferHeight = height;
}

// ---------- Action buttons / state advance ----------

void clearActionButtons() {
  for (const auto button : g_app.actionButtons) DestroyWindow(button);
  g_app.actionButtons.clear();
}

void rebuildActionButtons(HWND window, int clientWidth, int clientHeight) {
  clearActionButtons();
  if (!g_app.hasRound || g_app.watchMode || !g_app.humanSeat) return;
  int y = kToolbarHeight + kStatusHeight + 48;
  const int x = clientWidth - kActionPanelWidth + 12;
  const int w = kActionPanelWidth - 24;
  for (std::size_t index = 0; index < g_app.currentActions.size(); ++index) {
    if (y + kActionButtonHeight > clientHeight - 10) break;
    const auto label = actionLabel(g_app.currentActions[index], g_app.round);
    HWND button = CreateWindowExA(
      0, "BUTTON", label.c_str(),
      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
      x, y, w, kActionButtonHeight,
      window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdActionBase + index)),
      GetModuleHandleA(nullptr), nullptr);
    if (g_app.actionFont) SendMessageA(button, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.actionFont), TRUE);
    g_app.actionButtons.push_back(button);
    y += kActionButtonHeight + kActionButtonGap;
  }
}

void advanceAiUntilHumanPrompt() {
  if (!g_app.hasRound || !g_app.humanSeat || g_app.watchMode) return;
  for (int step = 0; step < 200; ++step) {
    if (g_app.round.phase == mahjong::Phase::Finished) return;
    auto humanActions = mahjong::getLegalActions(g_app.round, *g_app.humanSeat);
    if (!humanActions.empty()) {
      // Auto-draw: drawing isn't a strategic decision. If Draw is the only
      // legal action for the human, apply it immediately so the player never
      // has to click a Draw button.
      if (humanActions.size() == 1 && humanActions.front().type == mahjong::ActionType::Draw) {
        try {
          g_app.round = applyAction(g_app.round, *g_app.humanSeat, humanActions.front());
          continue;
        } catch (...) { return; }
      }
      return;
    }
    g_app.round = mahjong::advanceAiRound(g_app.round, g_app.difficulty, g_app.round.wall.seed).state;
  }
}

void refresh(HWND window) {
  if (!g_app.hasRound) g_app.currentActions.clear();
  else if (g_app.humanSeat && !g_app.watchMode) g_app.currentActions = mahjong::getLegalActions(g_app.round, *g_app.humanSeat);
  else g_app.currentActions.clear();

  std::string status;
  if (!g_app.statusOverride.empty()) status = g_app.statusOverride;
  else if (!g_app.hasRound) status = "Choose a mode to start playing.";
  else if (g_app.watchMode) status = "Watching four AI players. Use Step or Auto to End.";
  else if (g_app.round.phase == mahjong::Phase::Finished) status = "Round finished. Click Next Round to continue.";
  else if (!g_app.currentActions.empty()) {
    if (currentlyAwaitingHumanDiscard()) status = "Your turn: click a tile to discard, or use an action button.";
    else status = "Your prompt: choose a legal action.";
  } else status = "Waiting on AI. Use Step or Auto to End if needed.";
  SetWindowTextA(g_app.statusView, status.c_str());

  RECT client{};
  GetClientRect(window, &client);
  rebuildActionButtons(window, client.right, client.bottom);
  InvalidateRect(window, nullptr, FALSE);
}

void startHumanGame(HWND window, int humanSeat) {
  syncDifficulty();
  syncMinimumFan();
  g_app.watchMode = false;
  g_app.humanSeat = humanSeat;
  g_app.viewerSeat = humanSeat;
  g_app.revealAll = false;
  g_app.statusOverride.clear();
  std::vector<mahjong::Controller> controllers(4, mahjong::Controller::Ai);
  controllers[static_cast<std::size_t>(humanSeat)] = mahjong::Controller::Human;
  std::vector<std::string> names{"AI East", "AI South", "AI West", "AI North"};
  names[static_cast<std::size_t>(humanSeat)] = "You";
  g_app.round = mahjong::createInitialRoundState(timestampSeed("gui-human"), 0, mahjong::Wind::East, std::nullopt, selectedRules(), controllers, names);
  g_app.hasRound = true;
  advanceAiUntilHumanPrompt();
  refresh(window);
}

void startWatchAi(HWND window) {
  syncDifficulty();
  syncMinimumFan();
  g_app.watchMode = true;
  g_app.humanSeat.reset();
  g_app.viewerSeat = 0;
  g_app.revealAll = true;
  g_app.statusOverride.clear();
  g_app.round = mahjong::createInitialRoundState(timestampSeed("gui-four-ai"), 0, mahjong::Wind::East, std::nullopt, selectedRules());
  g_app.hasRound = true;
  refresh(window);
}

void stepAi(HWND window) {
  if (!g_app.hasRound || g_app.round.phase == mahjong::Phase::Finished) return;
  syncDifficulty();
  syncMinimumFan();
  g_app.round = mahjong::advanceAiRound(g_app.round, g_app.difficulty, g_app.round.wall.seed).state;
  if (!g_app.watchMode) advanceAiUntilHumanPrompt();
  refresh(window);
}

void autoToEnd(HWND window) {
  if (!g_app.hasRound) return;
  syncDifficulty();
  syncMinimumFan();
  for (int step = 0; step < 1500 && g_app.round.phase != mahjong::Phase::Finished; ++step) {
    if (!g_app.watchMode && g_app.humanSeat && !mahjong::getLegalActions(g_app.round, *g_app.humanSeat).empty()) break;
    g_app.round = mahjong::advanceAiRound(g_app.round, g_app.difficulty, g_app.round.wall.seed).state;
  }
  refresh(window);
}

void nextRound(HWND window) {
  if (!g_app.hasRound || g_app.round.phase != mahjong::Phase::Finished) return;
  syncMinimumFan();
  g_app.round = mahjong::createNextRoundState(g_app.round);
  if (!g_app.watchMode) advanceAiUntilHumanPrompt();
  refresh(window);
}

// ---------- WndProc ----------

void createFonts() {
  auto make = [](int height, int weight, const wchar_t* face) {
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_DONTCARE, face);
  };
  g_app.uiFont = make(15, FW_NORMAL, L"Segoe UI");
  g_app.uiBoldFont = make(15, FW_SEMIBOLD, L"Segoe UI");
  g_app.tileBigFont = make(26, FW_BOLD, L"Microsoft JhengHei UI");
  g_app.tileNumberFont = make(26, FW_BOLD, L"Segoe UI");
  g_app.tileSmallFont = make(16, FW_BOLD, L"Microsoft JhengHei UI");
  g_app.tileIndexFont = make(10, FW_BOLD, L"Segoe UI");
  g_app.plaqueFont = make(14, FW_NORMAL, L"Segoe UI");
  g_app.plaqueLargeFont = make(18, FW_SEMIBOLD, L"Segoe UI");
  g_app.centerLargeFont = make(56, FW_BOLD, L"Microsoft JhengHei UI");
  g_app.actionFont = make(14, FW_NORMAL, L"Segoe UI");
}

void destroyFonts() {
  HFONT* fonts[] = {&g_app.uiFont, &g_app.uiBoldFont, &g_app.tileBigFont, &g_app.tileNumberFont,
                    &g_app.tileSmallFont, &g_app.tileIndexFont, &g_app.plaqueFont, &g_app.plaqueLargeFont,
                    &g_app.centerLargeFont, &g_app.actionFont};
  for (HFONT* f : fonts) {
    if (*f) { DeleteObject(*f); *f = nullptr; }
  }
}

void createToolbarControls(HWND window) {
  HMODULE inst = GetModuleHandleA(nullptr);
  const int y = 10;
  const int h = 30;
  auto addButton = [&](const char* text, int x, int w, int id) {
    HWND b = CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             x, y, w, h, window,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
    if (g_app.uiFont) SendMessageA(b, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.uiFont), TRUE);
    return b;
  };
  addButton("Play East", 10, 80, kIdNewEast);
  addButton("Play South", 95, 80, kIdNewSouth);
  addButton("Play West", 180, 80, kIdNewWest);
  addButton("Play North", 265, 80, kIdNewNorth);
  addButton("Watch 4 AI", 360, 90, kIdWatchAi);
  addButton("Step", 460, 60, kIdStep);
  addButton("Auto to End", 525, 90, kIdAuto);
  addButton("Next Round", 620, 90, kIdNextRound);
  addButton("Reveal", 715, 70, kIdReveal);

  g_app.autoAiButton = CreateWindowExA(0, "BUTTON", "Auto AI",
                                       WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                       1060, 14, 80, 22, window,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAutoToggle)), inst, nullptr);
  if (g_app.uiFont) SendMessageA(g_app.autoAiButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.uiFont), TRUE);

  HWND minFanLabel = CreateWindowExA(0, "STATIC", "Min Fan:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     795, 16, 60, 22, window, nullptr, inst, nullptr);
  if (g_app.uiFont) SendMessageA(minFanLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.uiFont), TRUE);
  g_app.minFanEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "3",
                                     WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_CENTER,
                                     855, 11, 44, 26, window,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdMinFan)), inst, nullptr);
  if (g_app.uiFont) SendMessageA(g_app.minFanEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.uiFont), TRUE);

  HWND aiLabel = CreateWindowExA(0, "STATIC", "AI:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 910, 16, 28, 22, window, nullptr, inst, nullptr);
  if (g_app.uiFont) SendMessageA(aiLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.uiFont), TRUE);
  g_app.difficultyCombo = CreateWindowExA(0, "COMBOBOX", "",
                                          WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                          940, 10, 110, 200, window,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDifficulty)),
                                          inst, nullptr);
  if (g_app.uiFont) SendMessageA(g_app.difficultyCombo, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.uiFont), TRUE);
  SendMessageA(g_app.difficultyCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Easy"));
  SendMessageA(g_app.difficultyCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Medium"));
  SendMessageA(g_app.difficultyCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Hard"));
  SendMessageA(g_app.difficultyCombo, CB_SETCURSEL, 1, 0);

  g_app.statusView = CreateWindowExA(0, "STATIC", "Choose a mode.",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                     10, kToolbarHeight + 4, 1200, kStatusHeight - 6,
                                     window, nullptr, inst, nullptr);
  if (g_app.uiFont) SendMessageA(g_app.statusView, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.uiFont), TRUE);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_CREATE: {
      createFonts();
      createToolbarControls(window);
      refresh(window);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(window, &ps);
      RECT client{};
      GetClientRect(window, &client);
      const int w = client.right;
      const int h = client.bottom;
      ensureBackbuffer(dc, w, h);
      HDC mem = CreateCompatibleDC(dc);
      HGDIOBJ oldBmp = SelectObject(mem, g_app.backbuffer);
      drawFrame(mem, w, h);
      BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
      SelectObject(mem, oldBmp);
      DeleteDC(mem);
      EndPaint(window, &ps);
      return 0;
    }
    case WM_SIZE: {
      RECT client{};
      GetClientRect(window, &client);
      rebuildActionButtons(window, client.right, client.bottom);
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!currentlyAwaitingHumanDiscard()) {
        if (g_app.hoverTileId) {
          g_app.hoverTileId.reset();
          InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
      }
      POINT pt{LOWORD(lParam), HIWORD(lParam)};
      std::optional<std::string> hover;
      for (const auto& hit : g_app.handHits) {
        if (PtInRect(&hit.rect, pt) && tileIsDiscardable(hit.tileId)) {
          hover = hit.tileId;
          break;
        }
      }
      if (hover != g_app.hoverTileId) {
        g_app.hoverTileId = hover;
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    case WM_LBUTTONDOWN: {
      if (!currentlyAwaitingHumanDiscard()) return 0;
      POINT pt{LOWORD(lParam), HIWORD(lParam)};
      for (const auto& hit : g_app.handHits) {
        if (PtInRect(&hit.rect, pt) && tileIsDiscardable(hit.tileId)) {
          try {
            syncMinimumFan();
            g_app.round = mahjong::discardTile(g_app.round, hit.tileId);
            advanceAiUntilHumanPrompt();
            refresh(window);
          } catch (const std::exception& error) {
            MessageBoxA(window, error.what(), "Discard failed", MB_OK | MB_ICONERROR);
          }
          return 0;
        }
      }
      return 0;
    }
    case WM_TIMER: {
      if (wParam == kTimerAutoAi && g_app.autoAi) {
        if (g_app.hasRound && g_app.round.phase != mahjong::Phase::Finished) {
          if (g_app.watchMode) {
            stepAi(window);
          } else if (g_app.humanSeat) {
            const auto humanActions = mahjong::getLegalActions(g_app.round, *g_app.humanSeat);
            // Don't auto-act when the human can actually win, claim, or discard
            bool hasMeaningfulAction = false;
            for (const auto& a : humanActions) {
              if (a.type != mahjong::ActionType::Pass) { hasMeaningfulAction = true; break; }
            }
            if (!hasMeaningfulAction) {
              // Auto-pass any pass-only prompt, then step AI; loop tightly via timer
              if (!humanActions.empty()) {
                try {
                  syncMinimumFan();
                  g_app.round = applyAction(g_app.round, *g_app.humanSeat, humanActions.front());
                } catch (...) {}
              }
              advanceAiUntilHumanPrompt();
              refresh(window);
            }
          }
        }
      }
      return 0;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wParam);
      try {
        if (id == kIdNewEast) startHumanGame(window, 0);
        else if (id == kIdNewSouth) startHumanGame(window, 1);
        else if (id == kIdNewWest) startHumanGame(window, 2);
        else if (id == kIdNewNorth) startHumanGame(window, 3);
        else if (id == kIdWatchAi) startWatchAi(window);
        else if (id == kIdStep) stepAi(window);
        else if (id == kIdAuto) autoToEnd(window);
        else if (id == kIdNextRound) nextRound(window);
        else if (id == kIdReveal) {
          g_app.revealAll = !g_app.revealAll;
          refresh(window);
        } else if (id == kIdAutoToggle) {
          g_app.autoAi = (SendMessageA(g_app.autoAiButton, BM_GETCHECK, 0, 0) == BST_CHECKED);
          if (g_app.autoAi) {
            SetTimer(window, kTimerAutoAi, 1000, nullptr);
          } else {
            KillTimer(window, kTimerAutoAi);
          }
        } else if (id >= kIdActionBase && id < kIdActionBase + static_cast<int>(g_app.currentActions.size()) && g_app.humanSeat) {
          syncMinimumFan();
          g_app.round = applyAction(g_app.round, *g_app.humanSeat, g_app.currentActions[static_cast<std::size_t>(id - kIdActionBase)]);
          advanceAiUntilHumanPrompt();
          refresh(window);
        }
      } catch (const std::exception& error) {
        MessageBoxA(window, error.what(), "Action failed", MB_OK | MB_ICONERROR);
      }
      return 0;
    }
    case WM_DESTROY:
      KillTimer(window, kTimerAutoAi);
      clearActionButtons();
      if (g_app.backbuffer) { DeleteObject(g_app.backbuffer); g_app.backbuffer = nullptr; }
      destroyFonts();
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcA(window, message, wParam, lParam);
}

int runSmoke() {
  auto result = mahjong::runAiRoundSimulation("gui-smoke", mahjong::AiDifficulty::Medium, 1000);
  return result.completed ? 0 : 1;
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR commandLine, int showCommand) {
  const std::string args(commandLine ? commandLine : "");
  if (args.find("--smoke") != std::string::npos) return runSmoke();
  const bool demoMode = args.find("--demo") != std::string::npos;

  SetProcessDPIAware();

  const char className[] = "HongKongMahjongCppGui";
  WNDCLASSA windowClass{};
  windowClass.lpfnWndProc = windowProc;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = className;
  windowClass.hbrBackground = nullptr;
  windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
  windowClass.style = CS_HREDRAW | CS_VREDRAW;
  RegisterClassA(&windowClass);

  HWND window = CreateWindowExA(
    0, className, "Hong Kong Mahjong",
    WS_OVERLAPPEDWINDOW,
    CW_USEDEFAULT, CW_USEDEFAULT,
    1440, 920,
    nullptr, nullptr, instance, nullptr);
  if (!window) return 1;
  ShowWindow(window, showCommand);
  UpdateWindow(window);

  if (demoMode) {
    startWatchAi(window);
    for (int i = 0; i < 35; ++i) stepAi(window);
    InvalidateRect(window, nullptr, FALSE);
    UpdateWindow(window);
  }

  MSG message{};
  while (GetMessageA(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageA(&message);
  }
  return static_cast<int>(message.wParam);
}
