#include "server/room_server.hpp"

#include <iostream>

int main() {
  mahjong::server::RoomManager manager;
  const auto created = manager.createRoom("server-demo");
  const auto snapshot = manager.createSnapshot(created.room);
  std::cout << "Hong Kong Mahjong authoritative room shell\n";
  std::cout << "Room: " << snapshot.roomCode << " version " << snapshot.version << "\n";
  std::cout << "Phase: " << mahjong::toString(snapshot.phase) << ", live wall: " << snapshot.liveWallCount << "\n";
  std::cout << "Private seat claim links:\n";
  for (const auto& link : created.claimLinks) {
    std::cout << "  seat " << link.seatIndex << ": " << link.url << "\n";
  }
  return 0;
}
