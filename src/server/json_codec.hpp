#pragma once

#include "core/mahjong_core.hpp"
#include "server/room_server.hpp"

#include <nlohmann/json.hpp>

namespace mahjong::server::codec {

using nlohmann::json;

// Enum encoders (lower-case strings to feel natural in JSON).
std::string encode(Category category);
std::string encode(Suit suit);
std::string encode(Wind wind);
std::string encode(Dragon dragon);
std::string encode(Phase phase);
std::string encode(Controller controller);
std::string encode(MeldKind kind);
std::string encode(WinSource source);
std::string encode(ConclusionReason reason);
std::string encode(ActionType type);
std::string encode(KongType kongType);

// Returns std::nullopt when input does not match any known encoding.
std::optional<ActionType> decodeActionType(const std::string& value);
std::optional<KongType> decodeKongType(const std::string& value);
std::optional<WinSource> decodeWinSource(const std::string& value);

// Type-to-JSON encoders.
json toJson(const Tile& tile);
json toJson(const Meld& meld);
json toJson(const LastDiscard& discard);
json toJson(const LastDraw& draw);
json toJson(const RoundFanFeature& feature);
json toJson(const RoundPaymentLine& line);
json toJson(const RoundSettlement& settlement);
json toJson(const RoundConclusion& conclusion);
json toJson(const LegalAction& action);
json toJson(const PublicPlayerSnapshot& player);
json toJson(const RoomSeatRecord& seat);
json toJson(const RoomSnapshot& snapshot);
json toJson(const ClaimLink& link);

// Parse a LegalAction posted from a client. The result is a "blueprint" the
// server will look up against the seat's currently-legal action list.
std::optional<LegalAction> legalActionFromJson(const json& value);

} // namespace mahjong::server::codec
