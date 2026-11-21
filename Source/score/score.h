/**
 * @file score/score.h
 *
 * Multiplayer scoring and leader board.
 *
 * Three switches (see PLAN-scoring.md, "Flag everything"):
 * 1. Build option DEVILUTIONX_SCORING: without it this header only has empty inline stubs and score.cpp is not compiled.
 * 2. The game info byte GameData::scoring, set by the host from the Gameplay option "Scoring" (default off).
 * 3. ScoringActive(): compiled in, multiplayer, and the game info says so. Every hook starts with it.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "game_mode.hpp"

namespace devilution {

struct Item;
struct Monster;
struct Player;
struct TCmd;
struct Surface;
enum class DeathReason;

#ifdef DEVILUTIONX_SCORING

/** Compiled in, a multiplayer game, and the host turned scoring on. */
bool ScoringActive();
/** ScoringActive() and the game has not been scored yet (claims are still worth sending). */
bool ScoringOpen();

/** Host side: copies the "Scoring" option and whether the [Scoring] values differ from the defaults into the game info. */
void ScoreInitGameInfo(GameData &gameData);
/** After joining or creating a game. The creator starts the table with its own [Scoring] values. */
void ScoreOnGameStart(bool isCreator);
/** On leaving a game. */
void ScoreOnGameEnd();
/** A player's info was received and the player is now in the game (called on every client). */
void ScoreOnPlayerActive(const Player &player);
/** A player left the game (called before the player record is cleared). */
void ScoreOnPlayerLeft(const Player &player);
/** Once per game tick. */
void ScoreTick();

/**
 * @brief A monster died on this client.
 * @param creditedSlot The killer (this client's player, or the owner of the golem that killed it), or 0xFF for nobody
 *                     (trap, berserked monster); nobody-kills are claimed only for Diablo, by the level owner.
 */
void ScoreOnMonsterDeath(const Monster &monster, uint8_t creditedSlot);

/** Marks which player's damage is being applied to the local player right now (killing blow attribution). */
class ScoreDamageSource {
public:
	explicit ScoreDamageSource(uint8_t slot);
	~ScoreDamageSource();

private:
	uint8_t previous_;
};
/** The local player died (before CMD_PLRDEAD is sent). */
void ScoreOnLocalPlayerDeath(DeathReason deathReason);

/** The local player picked up gold from the floor; `amount` is what went into the inventory. */
void ScoreOnGoldPickedUp(const Item &pile, uint32_t amount);
/** The local player picked up a non-gold item from the floor. */
void ScoreOnItemPickedUp(const Item &item);
/** The local player put an item on the floor (drop, swap, inventory full, death): it is second-hand from now on. */
void ScoreOnPlayerDrop(const Item &item);
/** The local player drank a potion. */
void ScoreOnPotionDrunk(int miscId, bool lifeOrManaBelowMax);
/** An item of the local player went from unidentified to identified. */
void ScoreOnIdentified(const Item &item);

/** Network handlers (msg.cpp ParseCmd). Return the message size, 0 if invalid; `apply` is false while messages are being buffered. */
size_t ScoreOnClaimCmd(uint8_t pnum, const TCmd *cmd, size_t maxCmdSize, bool apply);
size_t ScoreOnAwardCmd(uint8_t pnum, const TCmd *cmd, size_t maxCmdSize, bool apply);
size_t ScoreOnTableCmd(uint8_t pnum, const TCmd *cmd, size_t maxCmdSize);

/** The "/score" chat command: the standings as places, into the chat. */
void ScoreChatCommand();
/** The keeper may end the game from the game menu. */
bool ScoreCanEndGame();
void ScoreEndGameByHost();

/** Lines for the F1 help: the rules with the game's point values. Empty when scoring is not active. */
std::vector<std::string> ScoreHelpLines();

/** The leader board / reveal overlay. */
extern bool ScoreBoardFlag;
void ToggleScoreBoard();
void DrawScoreBoard(const Surface &out);
void ScoreBoardKey(int direction);
/** One line per row for the test harness checkpoints (the whole table, counters and totals). */
std::string ScoreDescribeForHarness();

#else

inline bool ScoringActive() { return false; }
inline bool ScoringOpen() { return false; }
inline void ScoreInitGameInfo(GameData &) { }
inline void ScoreOnGameStart(bool) { }
inline void ScoreOnGameEnd() { }
inline void ScoreOnPlayerActive(const Player &) { }
inline void ScoreOnPlayerLeft(const Player &) { }
inline void ScoreTick() { }
inline void ScoreOnMonsterDeath(const Monster &, uint8_t) { }
class ScoreDamageSource {
public:
	explicit ScoreDamageSource(uint8_t) { }
};
inline void ScoreOnLocalPlayerDeath(DeathReason) { }
inline void ScoreOnGoldPickedUp(const Item &, uint32_t) { }
inline void ScoreOnItemPickedUp(const Item &) { }
inline void ScoreOnPlayerDrop(const Item &) { }
inline void ScoreOnPotionDrunk(int, bool) { }
inline void ScoreOnIdentified(const Item &) { }
inline bool ScoreCanEndGame() { return false; }
inline void ScoreEndGameByHost() { }
inline std::vector<std::string> ScoreHelpLines() { return {}; }
constexpr bool ScoreBoardFlag = false;

#endif

/**
 * @brief The single switch for "nobody is kicked out when Diablo dies" (Peter 2026-11-20).
 * Today it applies when scoring is on; returning gbIsMultiplayer here would make it general.
 */
bool DiabloDeathKeepsPlayersInGame();

} // namespace devilution
