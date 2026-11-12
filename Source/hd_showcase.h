/**
 * @file hd_showcase.h
 *
 * Renders level chunks (.dun files) for the HD pack tooling, headless. Peter's use: see every tile of a tileset as the engine draws it, before and after
 * a restyle. Driven by DEVILUTIONX_HD_SHOWCASE=<script> during a demo (a demo is what sets the resolution and ticks the game), with
 * DEVILUTIONX_HD_DUMP=<dir> (and DEVILUTIONX_HD_DUMP_EVERY=0) naming where the frames go. One command runs per game tick:
 *
 *   map <dun> <dungeon type 1-6> <x> <y> [palette]   warp to a chunk (an MPQ path, or an absolute path of a loose .dun), viewing square x, y
 *   view <x> <y>                                     centre the camera on a square
 *   wait <ticks>
 *   hero                                             from here on the player is drawn (give it before `map`)
 *   dim                                              from here on the level keeps the game's own lighting (give it before `map`)
 *   strip [hands|armour]                             the player takes off what they hold, what they wear, or both
 *   cast <spell id> <direction 0-7>                  the player casts a spell they know, three squares that way
 *   equip <head|chest|left|right> <item name>        the player puts on a plain item of that base name ("Chain Mail", "Short Sword")
 *   face <direction 0-7>                             the player stands facing that way
 *   attack <direction 0-7>                           the player swings at the next square that way
 *   walk <direction 0-7> [squares]                   the player walks that way (one square unless told)
 *   die [keep]                                       the player dies where they stand; a death in the dungeon drops everything worn, so
 *                                                    it plays the light armour's death: `keep` leaves the armour on (as a duel's death does)
 *   shot <name>                                      dump <name>.json (the camera) and hd-/low-/ids-<name>.png
 *   quit
 *
 * Every tick the level is fully lit, monsters are gone and the player is not drawn (unless `dim` and `hero` say otherwise).
 */
#pragma once

#include <string>

namespace devilution::hd_showcase {

/** Whether DEVILUTIONX_HD_SHOWCASE names a script. */
bool Active();

/** The chunk the `map` command asked for, and the palette to load after it ("" for the level's usual one). */
const std::string &MapPath();
const std::string &MapPalette();

/** Runs the next script command; called at the end of every game tick. */
void AfterGameTick();

} // namespace devilution::hd_showcase
