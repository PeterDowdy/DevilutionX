#pragma once

/**
 * HD presentation layer.
 *
 * The game keeps rendering at its own resolution into the 8-bit PalSurface; lighting, TRN recolours,
 * transparency and palette effects all stay as they are. Alongside it, the HD layer keeps a *provenance
 * plane*: for each 8-bit pixel, which HD image (if any) drew it, where that image's top-left was on screen,
 * the unlit colour index the art had there, and the index that was actually written.
 *
 * At presentation time each 8-bit pixel becomes an N x N block: sampled from the HD image when the pixel
 * still holds what the HD draw wrote, otherwise the pixel's live palette colour (nearest). HD colours are
 * graded per channel by live[written] / reference[source], where reference is the palette the art was
 * authored against: that carries lighting, recolours, fades and palette cycling over to true colour.
 *
 * Shading: the grade is never taken pixel by pixel from the original (a light table darkens each palette ramp by a different share, and a
 * recolour moves some indices and not others, so a per-pixel ratio stamps the 1997 picture into art drawn anew). Instead:
 *   - a sprite drawn through one light table (or none) - monsters, players, items, missiles, objects, the panels - takes one grade per
 *     draw, made from all the pixels it drew that are still on screen: per channel, the line through their (reference[source],
 *     live[written]) pairs above black, applied to the art and clamped at black. A light table steps each colour down its ramp, which takes
 *     about the same brightness off every colour and stops at black, so dim light crushes the shadows as it does in the original (a plain
 *     ratio would keep them in proportion); with too few pairs for a line, the sums' ratio;
 *   - a sprite drawn through a recolour (a TRN that is not a light table, or a type's TRN its art is graded through), blended, or lit by
 *     the per-pixel lightmap takes that grade over the pixels of the same draw within 2 original pixels (hd.cpp LocalRadius), so the
 *     recolour's regions carry over without the original's pixel pattern;
 *   - pillars, which the game lights piece by piece or pixel by pixel, take the grade of their light level, over all the pillar pixels of
 *     the frame lit at that level (and drawn with the same reference palette);
 *   - colours the level animates on its own (SetCycledColors: the lava and water of the caves, which colour cycling moves through the
 *     palette, and Hell's blood walls, which its light tables light apart from the ramps and rotate) are graded pixel by pixel, in any
 *     draw, and left out of every sum above: their (reference, live) pairs follow the animation, not the light, and pulled the grade of
 *     the rest of their draw or light level off (a Hell pillar's pixels drawn red and dark);
 *   - a sum lit to black grades to black (the per-pixel grade read a factor of 0 as "add the offset", so art lit to black kept most of
 *     its colour);
 *   - "shading pixel" in hd.txt keeps the old per-pixel grade everywhere.
 *
 * See-through (blended) draws: the game draws walls near the hero, and some sprites (the arches over a see-through wall, spell effects),
 * half over what is behind them: paletteTransparencyLookup, the palette colour nearest to the mean of the two. The index it writes is neither
 * the art's lit colour nor the one behind, so the layer neither grades by it nor shows the art alone: such a pixel (``blend``) keeps the index
 * the art was lit to (what the same draw would write opaque) and what was under it (its index and provenance, one layer deep). Its art is
 * graded by the lit index, as the opaque draw would be (and counts in its draw's or light level's grade by it), and composed half over the HD
 * block of what was behind (that art graded as its own draw, or the plain colour where there was none): the same rule, in true colour and
 * without the rounding to the palette. Before this the pair (art, blended index) entered the grades: art of colours of its own (anything not
 * the 1997 palette) came out speckled yellow and purple, and the wall drawn whole, not see-through. A pixel blended over a blended pixel keeps
 * that one as its art drawn opaque (one layer deep).
 *   - "minbright a <0..1>" in hd.txt (or DEVILUTIONX_HD_MINBRIGHT="a:<0..1>") keeps monsters readable in dim light: after the grade, no channel
 *     of a monster's art is darker than its own unlit value times that strength. It fades out over light levels 12-15, so a monster lit to
 *     black stays black. Off without the line.
 *
 * Pack layout (a folder, given by DEVILUTIONX_HD_PACK):
 *   hd.txt                                    "scale N", then one "image <file> <palette file>" per line, and optionally
 *                                             "silhouette free" (see below)
 *   sprite/<mpq path without ext>/<group>.<frame>.png   a CEL/CL2 frame, any size (sampled proportionally)
 *   sprite/<mpq path without ext>@<swap>/<group>.<frame>.png   the frame in a palette swap of its own: a monster
 *                                             type's TRN ("@bluered"), a unique monster's on top ("@bluered+rcrn",
 *                                             or "@+eth" on a type without one). Drawn as it is where the pack has
 *                                             it; otherwise the frame without the swap is graded as usual.
 *                                             Several unique monsters share one TRN (and some one type as well), so
 *                                             each may have art that is its alone, under its name after a "~":
 *                                             "@+general~moonbender", "@thinv3+general~bonesaw-the-litch" (the name
 *                                             in unique_monstdat.tsv in lower case, letters and digits kept, each run
 *                                             of anything else a "-"). That art is looked for first; a unique without
 *                                             it shows the art its TRN's monsters share ("@+general").
 *   sprite/<mpq path without ext>@c<NNN>/<group>.<frame>.png   a dropped item's own art: every item of a drop class shares one
 *                                             drop animation ("items/swrdflip" for all swords), so each item graphic (its inventory
 *                                             cursor graphic, three digits: "items/swrdflip@c057" for the Bastard Sword's) may have
 *                                             art of its own for that animation. Looked for first while the item draws (ItemGraphic);
 *                                             an item without it shows the drop class's art ("items/swrdflip").
 *   sprite/<mpq path without ext>@c<NNN>~<slug>/<group>.<frame>.png   the art of one unique item, over its item graphic's: the
 *                                             same <NNN> and the unique item's name in unique_itemdat.tsv in lower case, letters and
 *                                             digits kept, each run of anything else a "-" ("items/swrdflip@c057~the-butcher-s-cleaver" is
 *                                             the drop animation, "data/inv/objcurs@c057~the-butcher-s-cleaver/0.68.png" the inventory,
 *                                             belt, stash, store and cursor picture; <NNN> is the unique's own item graphic, and the
 *                                             inventory frame number is NNN + 11 as for any item). Looked for first; a unique without it
 *                                             shows what its item graphic shows. A unique item that shares its base item's picture
 *                                             shows it only once identified (the 1997 game gives nothing away before); one with a
 *                                             picture of its own of its own accord.
 *   held <dir> (a line of hd.txt)             where the art of heroes holding one item each is ("held held" for <pack>/held; an absolute
 *                                             path, or one relative to the pack; DEVILUTIONX_HD_HELD overrides it): a hero's sprite
 *                                             frames drawn with the very weapon it holds, in place of its weapon set's art that every
 *                                             item of the class shares. <dir>/<key>/render.json says what one item's folder has
 *                                             ("sprites": {"plrgfx/warrior/wls/wlsas": {"dirs": [0, ..., 7], "frames": 16}, ...}, the
 *                                             rest ignored; a folder without it is unfinished and left alone), and
 *                                             <dir>/<key>/plrgfx/<class>/<set>/<anim>/<group>.<frame>.png are the frames, named,
 *                                             boxed ("hdbox") and filtered as the frames under sprite/plrgfx/ are. <key> is the
 *                                             item's picture as for a dropped item, "c<NNN>", or a unique item's "c<NNN>~<slug>".
 *                                             Looked for first while a hero draws (HeldItem): the unique's art, then its picture's,
 *                                             then the weapon set's as before. Weapons only: a hero holding a shield keeps the set's
 *                                             art, unless the pair's art below shows. Effect layers stay those of the set's frame; the
 *                                             reference palette is the set's frame's. The folders are read when the pack loads; nothing
 *                                             of them is listed in hd.txt.
 *   pairs <dir> (a line of hd.txt)            where the art of heroes holding a weapon and a shield is, or a shield alone ("pairs pairs" for
 *                                             <pack>/pairs; DEVILUTIONX_HD_PAIRS overrides it): laid out as the held folders are (render.json,
 *                                             then <dir>/<key>/plrgfx/...), <key> "<weapon>+<shield>" ("c057+c083", "c131+c083~the-deflector",
 *                                             "c057~doombringer+c113"), each side named as for held art, or "<shield>" alone ("c083": a hero
 *                                             with a shield and no weapon it can use). Looked for before the held art: the pair with both
 *                                             uniques' own names, the weapon's unique with the shield's picture, the weapon's picture with the
 *                                             shield's unique, both pictures; then the weapon's held art; then the set's.
 *
 * Hero art (SetHeroArt, the "Hero Art" option): which of these a hero is drawn with. Original: no HD art for heroes at all (the 1997
 * sprites; the rest of the pack as before). Scaled up: the weapon set's art ("sprite/plrgfx/..."). Unique weapons: the weapon's held art
 * first (a hero with a shield keeps the set's art). Unique weapons and shields (the default): the pair's art first, then the weapon's,
 * then the set's. Each level falls back on the next one down wherever the pack lacks a folder, an item's key or a frame.
 *   sprite/<mpq path without ext>@hf/<group>.<frame>.png   a menu PCX that a Hellfire archive supplies under Diablo's path
 *                                             (ui_art/heros: Hellfire adds the monk as frame 3, Diablo's new hero): Hellfire's
 *                                             art only, it never falls back to the plain path's (Diablo's) art.
 *   tile/<tileset>/<pillar>.png               a level pillar (.min entry): 64 x 32*rows original pixels
 *   palettes/<mpq path>.pal                   768-byte RGB reference palettes
 *   fx/<mpq path without ext>/<group>.<frame>.png   an effect layer for that sprite frame (a spell cast's glow), see below
 *
 * "silhouette free": sprite art is no longer bound to the footprint of the original sprite. Where it is opaque
 * outside the footprint it is drawn there (the pixel takes the lighting of the nearest pixel of the sprite), and
 * where it is transparent inside the footprint what was behind the sprite shows instead of the original pixel,
 * HD art of whatever was behind included (one layer deep). Without the line, art shows only where the original
 * sprite has pixels, and transparent art shows the original pixel. Pillars (tiles) are not affected.
 *
 * Effect layers ("fx/..."): light that a frame's art gives off, drawn in true colour over the composed frame wherever the sprite is drawn in
 * the game view. The file holds two pictures side by side: the left half is laid over the frame by its alpha, the right half is added to it
 * (black adds nothing). Its tEXt chunk "hdfx" = "<dx> <dy> <w> <h>" places it in original pixels relative to the sprite's top-left, so it can
 * reach beyond the sprite's own rectangle (above it, too); the picture is sampled proportionally over that rectangle. Neither half is shaded
 * by the game's lighting (it is light), only by palette fades. Whatever is drawn after the sprite (a wall in front, a later sprite, the
 * panels, the cursor) covers the layer as it covers the sprite; parts of the effect behind the figure are left out of the file by whoever
 * makes it. A frame with an effect layer is also free of the limit on how far outside the original footprint its art may be drawn
 * (silhouette free), since a cast's pose is not bound to the original's. A palette swap's art ("sprite/<path>@<swap>/...") may have a layer
 * of its own ("fx/<path>@<swap>/...", with its own "hdfx"), which then replaces the body's for that swap; a swap without one shows the body's.
 *
 * Art larger than its sprite ("hdbox"): a sprite frame's picture may cover a rectangle other than the sprite's own, so that a figure that
 * reaches past the original's canvas (a raised sword's tip, a foot) is drawn whole instead of being cut at it. Its tEXt chunk "hdbox" =
 * "<dx> <dy> <w> <h>" is the rectangle the picture covers, in original pixels relative to the sprite's top-left ("-8 -12 112 108" on a
 * 96 x 96 sprite: 8 more on either side, 12 more on top; words after the four numbers are for tools and ignored here); the picture is
 * sampled proportionally over it. Inside the sprite's footprint
 * nothing else changes. With "silhouette free" the art outside the footprint is drawn as that line describes, the part beyond the sprite's
 * rectangle included, and without the limit on how far from the sprite's own pixels (as for a frame with an effect layer); it is clipped
 * to the surface the sprite is drawn into, covered by whatever is drawn after the sprite, and absent while the sprite itself draws no
 * pixel (clipped away at the edge of the view). Without "silhouette free" a box only says where the picture sits. Effect layers stay
 * relative to the sprite's top-left. A frame without the chunk is drawn exactly as before.
 *
 * Props ("prop/..."): a level's free-standing things (a plant, a tree, stairs, a fence) drawn whole from a 3D render, not bound to the 1997
 * pillar's shape, and in front of or behind heroes, monsters, missiles and objects by where they stand. A pillar's art ("tile/...") is drawn only
 * inside the 1997 pillar's pixels, and the 1997 pixels decide what covers a hero; a model that doesn't match the 1997 outline is then cut off
 * where it goes beyond it, and floor shows where the 1997 prop was drawn and the model isn't. A prop is a picture of its own:
 *   prop/<tileset>/<pillar>.png               the prop of the pillar <pillar> (.min entry, as for tile/...), and per position, as tiles have
 *   prop/<tileset>/<pillar>@<x>-<y>.png       ("tileperiod") or
 *   prop/<tileset>/<pillar>@h<k>.png          ("tilehash"); a position without one of its own shows the pillar's prop.
 * Its tEXt chunk "hdprop" = "<dx> <dy> <w> <h> [foot <fx> <fy>] [keep]" says where it goes: the rectangle the picture covers, in original pixels
 * relative to the top-left of the pillar's own picture (tile/..., 64 x 32 * rows: the pillar's square is the diamond in its bottom 32 rows),
 * any size and anywhere (the picture is sampled proportionally; at scale N it is w * N x h * N to be pixel for pixel). Words after these
 * are for tools and ignored. "foot": the point the prop stands on, in the same coordinates (default: the centre of the pillar's own square):
 * the prop is drawn in the place of the dungeon square under that point in the game's back-to-front drawing of the level (DrawDungeon, square
 * by square): after the squares behind it and everything standing on them, before what stands on its square or in front of it. So a hero
 * behind the prop is covered by it as far as it reaches, one in front is drawn over it; where it is transparent what is behind it shows
 * (in HD, blended by its alpha; one layer deep, as for free silhouettes), and foliage drawn later goes under it. A big prop that a hero walks
 * into (stairs under a ribcage) is best cut into one prop per square, each picture holding the part of the model standing over its square.
 * "keep": the pillar is drawn as before (its art inside its 1997 shape) and the prop over it in its place (a lamp on a wall). Without it the
 * pillar's 1997 pieces above its floor are not drawn at all (nor its foliage), only its floor diamond with its tile art (in the floor's pass,
 * so what stands behind it is not cut off there): the prop alone stands on the square. hd.txt "propfloor <tileset> <pillar> ..." (a pillar,
 * or a range "304-307") does the same for pillars without a prop of their own (the other squares of a prop drawn whole from one).
 * Lighting: the prop is lit as the floor it stands on: by the light of its foot's square, or with per-pixel lighting column by column by the
 * light of the floor at its foot's row; it is graded by that light level's grade of its tileset's reference palette, as the pillars are. The
 * game's 8-bit frame shows it too: each of its pixels the colour of the reference palette nearest its art's (no colour the level animates),
 * through the light table. See-through: a prop is drawn half over what is behind it while its pillar would be (a wall near the hero), else
 * opaque. A tileset without any "prop/" file or "propfloor" line draws exactly as before.
 *
 * Memory: an image is loaded when a sprite frame (or pillar) is first drawn with it, and belongs to the sprite data it was registered for
 * (RegisterSprites): when the game frees that data (a hero's graphics swapped for another weapon or armour, a level's monsters, missiles),
 * the images only it held are let go, as soon as no pixel of the screen (or of a tracked off-screen buffer, or an effect layer) still shows
 * them; drawn again, they are loaded again, the same. Pillars belong to their tileset, an effect layer to the art it lights. As a safety net,
 * DEVILUTIONX_HD_MEMORY_MB (or "memory <MB>" in hd.txt) bounds what images that are still held but haven't been drawn for a while may take:
 * above it the longest unused are let go first (0: no bound). Images of 256 colours or fewer (the pack's pixel-filtered art) are kept as
 * 8-bit indices into a palette of their own colours, a quarter of the memory; others in full colour. Either way every pixel draws exactly as
 * it would from full colour. DEVILUTIONX_HD_UNLOAD=0 and DEVILUTIONX_HD_INDEXED=0 turn the two off (for comparisons);
 * DEVILUTIONX_HD_MEMLOG=<file> logs what the images take every 300 presents.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/clx_sprite.hpp"
#include "engine/point.hpp"
#include "engine/surface.hpp"

struct SDL_Surface;
struct SDL_Renderer;

namespace devilution::hd {

/** Which art heroes are drawn with (see "Hero art" at the top of this file); the values are those of the "Hero Art" option. */
enum class HeroArt : uint8_t {
	Original = 0,                ///< the 1997 sprites: no HD art for heroes
	ScaledUp = 1,                ///< the weapon set's art: every item of a weapon class alike
	UniqueWeapons = 2,           ///< each weapon's own (held art), then the set's
	UniqueWeaponsAndShields = 3, ///< each weapon and shield pair's own, then the weapon's, then the set's
};

struct PixelProvenance {
	uint32_t image;  ///< 1-based HD image id, 0 when the pixel was drawn by art without HD
	int16_t left;    ///< where the image's top-left was, in PalSurface coordinates
	int16_t top;
	uint8_t source;  ///< the art's own colour index, before lighting, TRN or blending
	uint8_t written; ///< the index written to the PalSurface
	uint8_t free : 2;  ///< drawn with a free silhouette: what was under the pixel is kept (see hd.cpp); 2 = added outside the sprite's footprint
	uint8_t under : 1; ///< drawn as foliage (TrackUnder): ground cover, which effect layers stay on top of
	uint8_t shade : 2; ///< how the art is graded (see the top of this file): 0 per draw, 1 over the pixels of its draw nearby, 2 per pixel,
	                   ///< 3 by light level (pillars)
	uint8_t light : 4; ///< the light level the game lit the pixel with (0 full light .. 15 dark), where the draw knows it (pillars)
	uint8_t blend : 1; ///< drawn half over what was behind it (see "See-through" at the top of this file; MarkBlend)
	uint32_t serial;   ///< which draw wrote the pixel
};

/**
 * Set while a tracked draw into the PalSurface runs: the plane, and the PalSurface pixel it starts at. Per thread: the
 * loading thread draws art into surfaces of its own (the virtual gamepad's potions) while the main thread draws and
 * presents the loading screen, and a draw on the loader must never be recorded in the main thread's plane.
 */
extern thread_local PixelProvenance *TrackPlane;
extern thread_local const uint8_t *TrackBase;
extern thread_local uint32_t TrackImage;
extern thread_local int16_t TrackLeft;
extern thread_local int16_t TrackTop;
extern thread_local uint8_t TrackFree;
extern thread_local uint32_t TrackSerial;
/**
 * Set while a floor tile's foliage draws (DrawCell, FoliageScope). Foliage is ground cover drawn after the sprites of its row; the original
 * sprites never reach into the next tile, but free-silhouette art does, and the foliage of the tile beside it (grass, a house's shadow) then
 * cut notches out of it. Pixels a sprite added outside its footprint (free == 2) therefore stay on top of foliage; its own pixels do not.
 */
extern thread_local uint8_t TrackUnder;
/** How the draw in progress is graded (PixelProvenance::shade). */
extern thread_local uint8_t TrackShade;
/** Set while a tracked draw into the PalSurface itself runs (not an off-screen buffer): its blended pixels keep what was behind them. */
extern thread_local bool TrackBlend;
/** PixelProvenance::shade of a draw graded by light level: it alone needs each pixel's light level (MarkLights, LightLevelOf). */
constexpr uint8_t ShadeByLevel = 3;

/** Record ``n`` pixels just written at ``dst`` from the art indices ``src``, lit with the light level ``light`` (0 when not known). */
inline void Mark(const uint8_t *dst, const uint8_t *src, unsigned n, uint8_t light = 0)
{
	if (TrackPlane == nullptr) return;
	PixelProvenance *p = TrackPlane + (dst - TrackBase);
	for (unsigned i = 0; i < n; ++i) {
		if (TrackUnder != 0 && p[i].free == 2) {
			const_cast<uint8_t *>(dst)[i] = p[i].written; // foliage goes under a free sprite's extension
			continue;
		}
		p[i] = { TrackImage, TrackLeft, TrackTop, src[i], dst[i], TrackFree, TrackUnder, TrackShade, light, 0, TrackSerial };
	}
}

/** Mark, each pixel lit with its own light level from ``lights`` (the per-pixel lightmap). */
inline void MarkLights(const uint8_t *dst, const uint8_t *src, unsigned n, const uint8_t *lights)
{
	if (TrackPlane == nullptr) return;
	PixelProvenance *p = TrackPlane + (dst - TrackBase);
	for (unsigned i = 0; i < n; ++i) {
		if (TrackUnder != 0 && p[i].free == 2) {
			const_cast<uint8_t *>(dst)[i] = p[i].written;
			continue;
		}
		p[i] = { TrackImage, TrackLeft, TrackTop, src[i], dst[i], TrackFree, TrackUnder, TrackShade, static_cast<uint8_t>(lights[i] & 15), 0, TrackSerial };
	}
}

/** The light level of ``table`` among the game's light tables (SetLightTables), 0 for any other. */
uint8_t LightLevelOf(const uint8_t *table);

/** Record ``n`` pixels just written at ``dst`` from one art index. */
inline void MarkFill(const uint8_t *dst, uint8_t color, unsigned n)
{
	if (TrackPlane == nullptr) return;
	PixelProvenance *p = TrackPlane + (dst - TrackBase);
	for (unsigned i = 0; i < n; ++i) {
		if (TrackUnder != 0 && p[i].free == 2) {
			const_cast<uint8_t *>(dst)[i] = p[i].written; // foliage goes under a free sprite's extension
			continue;
		}
		p[i] = { TrackImage, TrackLeft, TrackTop, color, dst[i], TrackFree, TrackUnder, TrackShade, 0, 0, TrackSerial };
	}
}

/** BeforeBlend, when the draw tracks blended pixels. */
void KeepBehind(const uint8_t *dst, unsigned n);

/**
 * Call before ``n`` pixels at ``dst`` are blended over what is there (paletteTransparencyLookup): keeps their indices and provenance as what is
 * behind them. Then Mark (or MarkLights, MarkFill) them as usual, and MarkBlend.
 */
inline void BeforeBlend(const uint8_t *dst, unsigned n)
{
	if (TrackBlend) KeepBehind(dst, n);
}

/** The ``n`` pixels at ``dst`` just marked were blended (see BeforeBlend); ``lit``: the index each would have been drawn as opaque. */
void MarkBlend(const uint8_t *dst, const uint8_t *lit, unsigned n);

/** Whether an HD pack is loaded. */
bool Enabled();

/** Loads the pack named by DEVILUTIONX_HD_PACK, if any. */
void Init();

/** The pack's scale ("scale N" in its hd.txt), read without loading it; 1 when there is no pack. For sizing the window before Init. */
int PackScale();

/**
 * The light radius a monster type carries in this pack ("light monsters/<sprite path>[@<trn>] <radius>" in hd.txt; e.g. a creature
 * with burning wings): 0 when the pack names none. ``spritePath`` is monstdat's assetsSuffix ("mega\mega"), ``trnFile`` its TRN
 * ("mega\balr", or empty): the swap's own line first, then the body's.
 */
int MonsterLight(std::string_view spritePath, std::string_view trnFile);

/**
 * The light radius the pack gives one unique monster of his own, under the keys of his own art: "light monsters/<sprite path>@<type
 * trn>+<unique trn>~<slug> <radius>" (his alone) first, then "...@<type trn>+<unique trn>" (the uniques of that type that share his TRN);
 * "@+<unique trn>..." for a type without a TRN. 0 when the pack names neither: then his type's line applies (MonsterLight), as his art falls
 * back on his type's. A unique's line never lights his type: MonsterLight doesn't read it. ``uniqueTrn`` is his TRN ("blf"), ``uniqueName``
 * the table's name ("Blightfire", made a slug as NameTrn does).
 */
int UniqueMonsterLight(std::string_view spritePath, std::string_view trnFile, std::string_view uniqueTrn, std::string_view uniqueName);

/**
 * Remembers which file, group and frame each sprite of a freshly loaded CEL/CL2 file is. ``path`` may carry an extension. ``owner``: the start
 * of the data the game will free (ClxFreeHook, ForgetSprites), when the sprites are part of a larger buffer (a sheet read from several files,
 * a monster type's animations read together); by default the sprites' own data.
 */
void RegisterSprites(std::string_view path, ClxSpriteListOrSheet sprites, const void *owner = nullptr);
void RegisterSpriteList(std::string_view path, uint16_t group, ClxSpriteList list, const void *owner = nullptr);

/**
 * The game frees the sprite data registered with ``owner``: forgets its sprites, and lets go of the HD images only they held (see "Memory" at
 * the top of this file). Owned CLX lists and sheets call it as they are freed (ClxFreeHook); other buffers (monster.cpp) call it themselves.
 */
void ForgetSprites(const void *owner);

/** What the HD images take (see "Memory" at the top of this file). */
struct MemoryUse {
	size_t bytes = 0;      ///< the loaded images' pixels (and free-silhouette reach) now
	size_t peak = 0;       ///< the most they took at once
	uint32_t images = 0;   ///< images loaded now
	uint32_t indexed = 0;  ///< of those, kept as 8-bit indices
	uint32_t loads = 0;    ///< images loaded since the pack loaded (again after being let go included)
	uint32_t unloads = 0;  ///< images let go since the pack loaded
};
MemoryUse Memory();
/** The same for the pack image ``file`` alone ("sprite/<path>/<group>.<frame>.png"): all 0 when it isn't loaded. */
MemoryUse Memory(std::string_view file);

/** Whether the pixels of the pack image ``file`` ("sprite/<path>/<group>.<frame>.png") are loaded now. */
bool Loaded(std::string_view file);

/**
 * ClxApplyTrans recolours sprites in place. For a sprite with HD art the layer keeps its original pixels and
 * the recolour, so draws can still record the art's own colour indices (see SpriteScope::original).
 */
void BeforeRecolour(ClxSprite sprite, const uint8_t *trn);
void AfterRecolour(ClxSprite sprite);

/** While it lives, names the TRN that ClxApplyTrans applies (a monster type's: "bluered"), for "@<swap>" art. */
class NamedRecolour {
public:
	explicit NamedRecolour(std::string_view name);
	~NamedRecolour();
};

/**
 * Names a TRN that sprites are drawn through (a unique monster's: "rcrn"), for "@<swap>+<name>" art, and with ``owner`` the one monster
 * whose TRN it is ("Bonesaw the Litch"), for that monster's own "@<swap>+<name>~<owner in lower case, with dashes>" art.
 */
void NameTrn(const uint8_t *trn, std::string_view name, std::string_view owner = {});

/**
 * Off-screen 8-bit buffers the game composes once and then copies to the screen (the control panel's BottomBuffer, the flask buffers): draws
 * into a registered buffer are tracked in a plane of its own, and Surface blits from it into the PalSurface carry that provenance along, so the
 * panel's art gets its HD image too. Blits from a buffer the layer does not track clear the provenance they cover. Forget a buffer before it is
 * freed.
 */
void TrackOffscreen(SDL_Surface *buffer);
void ForgetOffscreen(SDL_Surface *buffer);

/**
 * While it lives, the sprite draws are those of a dropped item with the cursor graphic ``cursorGraphic`` (Item::_iCurs): a frame looks for
 * the art of that item graphic ("sprite/<path>@c<NNN>/...", see the pack layout above) before the art its drop animation shares.
 */
class ItemGraphic {
public:
	explicit ItemGraphic(int cursorGraphic);
	/**
	 * An item drawn in the game world (``dropped``) or as an inventory picture. For a unique item whose own art may show, ``uniqueIndex``
	 * (its place in the unique item table, a cache key) and ``uniqueName`` (the table's English name): a frame looks for that unique's art
	 * ("@c<NNN>~<slug>") before all else. A pack without any of it costs nothing here. An inventory picture is the sprite's own frame, so
	 * only the unique's art is looked for then, not the item graphic's.
	 */
	ItemGraphic(int cursorGraphic, bool dropped, int uniqueIndex, std::string_view uniqueName);
	~ItemGraphic();
	ItemGraphic(const ItemGraphic &) = delete;
	ItemGraphic &operator=(const ItemGraphic &) = delete;
};

/** One item in a hero's hands: its item graphic (Item::_iCurs; -1: none), and for a unique whose own art may show its index and name. */
struct HeldPiece {
	int cursorGraphic = -1;
	int uniqueIndex = -1;
	std::string_view uniqueName;
};

/**
 * While it lives, the sprite draws are those of the hero ``who`` (any address that stays the hero's: its Player) holding the item with the
 * item graphic ``cursorGraphic`` (Item::_iCurs; -1: nothing that has held art, a shield, or a pair): a hero frame ("sprite/plrgfx/...")
 * looks for that item's held art ("held <dir>" above) before the weapon set's. For a unique item whose own art may show, ``uniqueIndex``
 * and ``uniqueName`` as for ItemGraphic: its "c<NNN>~<slug>" art is looked for first. With a ``shield`` too (the weapon may be none: a
 * shield alone), the pair's art ("pairs <dir>" above) is looked for before all; which of them at all, the hero art level says (SetHeroArt).
 *
 * It also notes what the hero holds now: when that differs from what it held when last noted, every frame of the hero lets go of the held
 * art it drew (see "Memory" at the top of this file), whether or not it is drawn again. A hero's sprite files are kept while it swaps two
 * items of one weapon class, so without this each frame would keep every item's art it ever drew until the file is freed. Construct one
 * (and let it go) where the hero's gear changes, too (CalcPlrInv). A pack without held art costs nothing here.
 */
class HeldItem {
public:
	HeldItem(const void *who, int cursorGraphic, int uniqueIndex = -1, std::string_view uniqueName = {});
	HeldItem(const void *who, HeldPiece weapon, HeldPiece shield);
	~HeldItem();
	HeldItem(const HeldItem &) = delete;
	HeldItem &operator=(const HeldItem &) = delete;
};

/** The frames of the hero ``who`` let go of the held art they drew (as when it holds another item; see HeldItem). */
void LetGoOfHeldArt(const void *who);

/**
 * The art the hero ``who`` was last noted holding (HeldItem), the most specific the pack has any of: "c057+c083" (a pair's),
 * "c057~doombringer", "c057", or nothing (the set's art).
 */
std::string HeldArt(const void *who);

/** The held-item and pair folders the pack loaded (render.json read), and the sprite files (kits) they have art for. */
struct HeldUse {
	uint32_t items = 0;
	uint32_t kits = 0;
	uint32_t frames = 0; ///< frames of the hero holding held or pair art now (all heroes)
	uint32_t pairs = 0;
	uint32_t pairKits = 0;
};
HeldUse Held();

/**
 * Sets which art heroes are drawn with (see "Hero art" at the top of this file). What heroes drew at the old level is let go of (loaded
 * again when drawn again), and what they hold is looked up again at their next draw. Kept across packs; set before Init too.
 */
void SetHeroArt(HeroArt level);
HeroArt GetHeroArt();

/** The 8-bit surface the game renders into (the one to track). */
void SetTarget(SDL_Surface *palSurface);

/**
 * Dumps the next presented frame as hd-<name>.png, low-<name>.png (and ids-<name>.png with DEVILUTIONX_HD_DUMP_IDS) into DEVILUTIONX_HD_DUMP,
 * whatever DEVILUTIONX_HD_DUMP_EVERY says (0 there turns the periodic dumps off). Does nothing without DEVILUTIONX_HD_DUMP.
 */
void RequestDump(std::string_view name);

/**
 * The game view is about to be drawn anew (DrawGame), or the screen was cleared: forgets the effect layers of the sprites drawn into the last
 * one. Until then they are composed into every presented frame, as the pixels of their sprites are.
 */
void NewScene();

/**
 * The game view ``out`` is about to be drawn anew: from now on, what drew its pixels before counts for nothing (a pixel in it not drawn again
 * shows its own colour). The game fills what it doesn't draw (unlit and unexplored tiles, the space outside the map) without the layer seeing
 * it, and those pixels mostly keep the very index (black) they held, so without this they kept showing art drawn there frames ago, and art kept
 * behind free silhouettes from frames ago.
 */
void ForgetView(const Surface &out);

/** The level tileset whose pillars DrawCell draws: "town", "hftown", "l1".."l6"; ``rows``: its pillars' rows of 32 pixels (for its props). */
void SetTileset(std::string_view name, int rows = 0);

/**
 * The palette indices ``first`` to ``last`` (none when first > last) are animated by the level (CycledColors): pixels whose art or drawn index
 * is one of them are graded pixel by pixel and left out of the grades of their draw and light level (see the top of this file).
 */
void SetCycledColors(uint8_t first, uint8_t last);

/**
 * The game's light tables (``size`` bytes from ``tables``): a sprite drawn through one of them is only lit, and takes one grade for the whole
 * draw; through any other TRN it is recoloured, and is graded locally (see the top of this file).
 */
void SetLightTables(const uint8_t *tables, size_t size);

/**
 * Tracks the pixels a CLX sprite draw writes (when it draws into the PalSurface). ``drawTrn``: the TRN the draw maps through. ``local``: the
 * draw's pixels don't share one light (lit by the per-pixel lightmap), so they are graded locally. A blended draw is graded as the same draw
 * opaque (see "See-through" at the top of this file).
 */
class SpriteScope {
public:
	SpriteScope(const Surface &out, Point position, ClxSprite clx, const uint8_t *drawTrn = nullptr, bool local = false);
	~SpriteScope();

	/** For a recoloured sprite with HD art: its pixel data before recolouring, and the recolour. */
	const uint8_t *original = nullptr;
	const uint8_t *trn = nullptr;
	/** For art of the draw's own swap: record the pixels' indices through ``drawTrn``, which the art already shows. */
	const uint8_t *marks = nullptr;
};

/**
 * The pack file of a pillar's art for one dungeon position (``tile``, dPiece coordinates), when hd.txt asks for art by position:
 * "tile/<tileset>/<pillar>@<x mod period>-<y mod period>.png" for ``period`` > 0 (a texture that repeats every ``period`` squares), else
 * "tile/<tileset>/<pillar>@h<k>.png" with k one of ``hashes`` picked from the position; empty when neither (or for no position).
 */
std::string TileVariantFile(std::string_view tileset, uint16_t pillar, Point tile, int period, int hashes);

/**
 * Tracks the pixels of one pillar: ``bottomLeft`` is the bottom-left pixel of its lowest pieces, ``tile`` the dungeon position it is drawn
 * at. Its art is the pack's art for that position when there is some (see TileVariantFile), else the pillar's own.
 */
class PillarScope {
public:
	PillarScope(const Surface &out, Point bottomLeft, uint16_t pillar, int rows, Point tile = { -1, -1 });
	~PillarScope();
};

/** A prop ("prop/...", see "Props" at the top of this file): where it is drawn, relative to its pillar. */
struct Prop {
	std::string file;
	int dx = 0, dy = 0, w = 0, h = 0; ///< the rectangle its picture covers, original pixels from the top-left of its pillar's picture
	int footX = 0, footY = 0;          ///< the point it stands on, the same way
	Displacement foot;                 ///< the square under that point, from the pillar's own square (dPiece coordinates)
	bool keep = false;                 ///< the pillar is drawn as well
};

/** Reads "hdprop" text (see "Props" at the top of this file) for a pillar of ``rows`` rows. False when it doesn't say where the prop goes. */
bool ParseProp(std::string_view text, int rows, Prop &prop);

/** The square under the point (``x``, ``y``) of a pillar's picture of ``rows`` rows, from the pillar's own square. */
Displacement SquareUnder(int x, int y, int rows);

/** Whether the tileset in use has props, or pillars whose pieces it leaves to props: without, nothing below is ever needed. */
bool HasProps();

/** Whether the pillar ``pillar`` of the tileset in use is drawn as its floor diamond alone, what stands on it left to props. */
bool PropFloorOnly(uint16_t pillar);

/** The squares, from their pillars, that the props of the tileset in use stand on (Prop::foot, each once). */
const std::vector<Displacement> &PropFeet();

/**
 * The prop of the pillar ``pillar`` drawn at the dungeon position ``tile`` (its art for that position first) when it stands on the square
 * ``foot`` from it; null when there is none, or it stands elsewhere.
 */
const Prop *PropAt(uint16_t pillar, Point tile, Displacement foot);

/**
 * Draws the prop ``prop`` of the pillar whose picture's top-left is ``pillarTopLeft`` (in ``out``) into ``out``, and its HD art: its pixels
 * through the light table of ``levels[i]`` (light level of its picture's column i; ``lightTables``: ``numLevels`` tables of 256 one after
 * the other), half over what is behind them through ``blend`` (paletteTransparencyLookup) unless it is null.
 */
void DrawProp(const Surface &out, const Prop &prop, Point pillarTopLeft, const uint8_t *levels, const uint8_t *lightTables, int numLevels,
    const uint8_t (*blend)[256] = nullptr);

/**
 * While it lives (inside the pillar's PillarScope), the pillar drawn at ``bottomLeft`` is drawn as its floor diamond alone (PropFloorOnly):
 * what its pieces draw outside the diamond in its bottom 32 rows is put back as it was, its HD art included; and the part of the diamond
 * its own pieces leave undrawn (the 1997 art had other pillars' pieces cover it, which are props' now) is drawn from its HD art, through
 * ``tbl`` (its light table), as its pieces would be.
 */
class FloorOnlyScope {
public:
	FloorOnlyScope(const Surface &out, Point bottomLeft, const uint8_t *tbl = nullptr);
	~FloorOnlyScope();
	FloorOnlyScope(const FloorOnlyScope &) = delete;
	FloorOnlyScope &operator=(const FloorOnlyScope &) = delete;

private:
	const Surface &out_;
	int x0_, y0_;
	const uint8_t *tbl_;
	uint32_t image_, serial_;
	int16_t left_, top_;
	std::vector<uint8_t> idx_;
	std::vector<PixelProvenance> prov_;
};

/** Whether the pixel (``x``, ``k`` rows up from the bottom row) of a pillar's bottom 32 rows is inside its floor diamond (x 0..63). */
bool InFloorDiamond(int x, int k);

/** While it lives, the pillar draw in progress is a floor tile's foliage (see TrackUnder). */
class FoliageScope {
public:
	FoliageScope() { TrackUnder = 1; }
	~FoliageScope() { TrackUnder = 0; }
	FoliageScope(const FoliageScope &) = delete;
	FoliageScope &operator=(const FoliageScope &) = delete;
};

/**
 * Composes the HD frame from ``palSurface`` and draws it with ``renderer`` (when not null).
 * Returns false when the HD layer is off, so the caller presents as usual.
 * ``dumpStep``: DEVILUTIONX_HD_DUMP names its frames by this (every DUMP_EVERY steps) instead of by the count of presents. The number of presents
 * per game tick differs from run to run, so only a number tied to the game itself (the demo's tick) lets frames of two builds be compared.
 */
bool Present(SDL_Renderer *renderer, SDL_Surface *palSurface, int dumpStep = -1);

} // namespace devilution::hd
