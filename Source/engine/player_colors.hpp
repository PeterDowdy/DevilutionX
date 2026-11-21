/**
 * @file engine/player_colors.hpp
 *
 * The colours that tell multiplayer players apart. Players 1-4 keep the original four (orange, yellow, red, blue);
 * players 5-8 add white, pink, lavender and khaki, all from the fixed part of the palette (indices 128-255 are the
 * same on every level). Used by the automap arrow, the hover outline and the experience tags.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/palette.h"

namespace devilution {

/** Number of players with a colour of their own; a player id at or above it falls back to player 1's colour. */
constexpr size_t NumPlayerColors = 8;

/** Automap arrow colour per player id (players 1-4 as in the original game). */
constexpr std::array<uint8_t, NumPlayerColors> PlayerMapColors = {
	PAL8_ORANGE + 1,
	PAL8_YELLOW + 1,
	PAL8_RED + 1,
	PAL8_BLUE + 1,
	PAL16_GRAY + 0,
	PAL8_RED + 0,
	PAL8_BLUE + 0,
	PAL16_YELLOW + 3,
};

/** Hover outline colour per player id (players 1-4 as in the original game). */
constexpr std::array<uint8_t, NumPlayerColors> PlayerOutlineColors = {
	PAL16_ORANGE + 7,
	PAL16_YELLOW + 7,
	PAL16_RED + 7,
	PAL16_BLUE + 7,
	PAL16_GRAY + 2,
	PAL16_RED + 1,
	PAL16_BLUE + 2,
	PAL16_YELLOW + 4,
};

/**
 * data/monstertags.clx has an empty tag (frame 0) and one tag each for players 1-4 (frames 1-4), filled with
 * PAL8_<colour> + 1..3. Players 5-8 draw player 1's frame through a translation that swaps its three orange shades
 * (light to dark) for three of their own.
 */
constexpr size_t PlayerTagFrame(size_t playerId)
{
	return playerId < 4 ? playerId + 1 : 1;
}

/** @return the translation for PlayerTagFrame(playerId), or nullptr when the frame is drawn as it is (players 1-4). */
inline const uint8_t *PlayerTagTranslation(size_t playerId)
{
	if (playerId < 4 || playerId >= NumPlayerColors)
		return nullptr;
	static constexpr uint8_t Shades[4][3] = {
		{ PAL16_GRAY + 0, PAL16_GRAY + 2, PAL16_GRAY + 4 },
		{ PAL8_RED + 0, PAL16_RED + 1, PAL16_RED + 3 },
		{ PAL8_BLUE + 0, PAL16_BLUE + 3, PAL16_BLUE + 5 },
		{ PAL16_YELLOW + 3, PAL16_YELLOW + 5, PAL16_YELLOW + 7 },
	};
	static const std::array<std::array<uint8_t, 256>, 4> Translations = [] {
		std::array<std::array<uint8_t, 256>, 4> translations {};
		for (size_t player = 0; player < 4; player++) {
			for (size_t i = 0; i < 256; i++)
				translations[player][i] = static_cast<uint8_t>(i);
			for (size_t shade = 0; shade < 3; shade++)
				translations[player][PAL8_ORANGE + 1 + shade] = Shades[player][shade];
		}
		return translations;
	}();
	return Translations[playerId - 4].data();
}

} // namespace devilution
