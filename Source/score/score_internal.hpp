/**
 * @file score/score_internal.hpp
 *
 * Multiplayer scoring: what the board (score_board.cpp) reads from the game side (score.cpp).
 */
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "score/score_rules.hpp"

namespace devilution {

/** The local copy of the score table, or nullptr before it has arrived. */
const score::ScoreTable *ScoreTableIfAny();
/** Family indices in dungeon order. */
const std::vector<uint8_t> &ScoreFamilyOrder();
/** Display name of a family (translated). */
std::string_view ScoreFamilyName(uint8_t family);
/** Opens the board on the stepped reveal (the scores just became final). */
void ScoreBoardStartReveal();
/** Closes the board and forgets its pages (leaving a game). */
void ScoreBoardReset();

} // namespace devilution
