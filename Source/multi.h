/**
 * @file multi.h
 *
 * Interface of functions for keeping multiplayer games in sync.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dvlnet/leaveinfo.hpp"
#include "game_mode.hpp"
#include "msg.h"
#include "utils/attributes.h"

namespace devilution {

using net::leaveinfo_t;

// Defined in player.h, forward declared here to allow for functions which operate in the context of a player.
struct Player;

// must be unsigned to generate unsigned comparisons with pnum
#define MAX_PLRS 8

/**
 * @brief The remaster's network protocol: the one value that decides which builds play together (0 would be stock
 * DevilutionX, which has no such id). 1 = 8 players (MAX_PLRS 8, eight portals in the join delta, unsigned hit mask).
 * Raise it whenever the wire format changes (new commands, delta layout, more players). It travels in the TCP join:
 * a joiner sends it as the join request's info, the host refuses a joiner without the same id before giving it a
 * player slot (a stock joiner sends an empty request), and the host appends it to the game info of the join accept,
 * so a joiner refuses a host without it (a stock host). See dvlnet/base.cpp ProtocolJoinTag.
 */
// 2 = the 8-player netcode together with multiplayer scoring (CMD_SCORE_*): raised when the two branches were merged,
// so a remaster build from before the merge is refused at the join.
constexpr uint8_t RemasterProtocol = 2;

/* @brief Contains info of running public game (for game list browsing) */
struct GameInfo {
	std::string name;
	GameData gameData;
	std::vector<std::string> players;
	std::optional<int> latency;
	std::optional<bool> peerIsRelayed;
};

extern bool gbSomebodyWonGameKludge;
extern uint16_t sgwPackPlrOffsetTbl[MAX_PLRS];
extern uint8_t gbActivePlayers;
extern bool gbGameDestroyed;
extern bool gbSelectProvider;
extern std::string GameName;
extern std::string GamePassword;
extern bool PublicGame;
extern uint8_t gbDeltaSender;
extern uint32_t player_state[MAX_PLRS];
extern bool IsLoopback;

void SwapGameDataLE(GameData &gameData);
DVL_API_FOR_TEST std::string DescribeLeaveReason(leaveinfo_t leaveReason);
std::string FormatGameSeed(const uint32_t gameSeed[4]);

void InitGameInfo();
void NetSendLoPri(uint8_t playerId, const std::byte *data, size_t size);
void NetSendHiPri(uint8_t playerId, const std::byte *data, size_t size);
void multi_send_msg_packet(uint32_t pmask, const std::byte *data, size_t size);
void multi_msg_countdown();
void multi_player_left(uint8_t pnum, leaveinfo_t reason);
void multi_net_ping();

/**
 * @return Always true for singleplayer
 */
bool multi_handle_delta();
void ProcessGameMessagePackets();
void multi_send_zero_packet(uint8_t pnum, _cmd_id bCmd, const std::byte *data, size_t size);
void NetClose();
bool NetInit(bool bSinglePlayer);
void recv_plrinfo(Player &player, const TCmdPlrInfoHdr &header, bool recv);

} // namespace devilution
