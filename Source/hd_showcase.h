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
 *   shot <name>                                      dump <name>.json (the camera) and hd-/low-/ids-<name>.png
 *   quit
 *
 * Every tick the level is fully lit, monsters are gone and the player is not drawn.
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
