/**
 * @file light_tables.hpp
 *
 * Declarations for light table data and generation.
 */
#pragma once

#include <array>
#include <cstdint>

#include "engine/lighting_defs.hpp"
#include "levels/gendung_defs.hpp"
#include "utils/attributes.h"

namespace devilution {

constexpr size_t NumLightRadiuses = 16;

extern DVL_API_FOR_TEST std::array<std::array<uint8_t, LightTableSize>, NumLightingLevels> LightTables;
/** @brief Contains a pointer to a light table that is fully lit (no color mapping is required). Can be null in hell. */
extern DVL_API_FOR_TEST uint8_t *FullyLitLightTable;
/** @brief Contains a pointer to a light table that is fully dark (every color result to 0/black). Can be null in hellfire levels. */
extern DVL_API_FOR_TEST uint8_t *FullyDarkLightTable;
extern uint8_t LightFalloffs[NumLightRadiuses][128];
extern uint8_t LightConeInterpolations[8][8][16][16];

void MakeLightTable();

/** A run of palette indices, first to last; none when first > last. */
struct PaletteRange {
	uint8_t first = 1;
	uint8_t last = 0;
};

/**
 * The palette indices that the level type ``type`` animates by colour cycling: the lava and water of the caves (and of Hellfire's crypt and
 * nest), which palette_update_caves (_crypt, _hive) move through the palette, and Hell's blood walls, which its light tables light apart from
 * the ramps (MakeLightTable) and lighting_color_cycling rotates. None for the other levels.
 */
PaletteRange CycledColors(dungeon_type type);

} // namespace devilution
