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
 *   - a sum lit to black grades to black (the per-pixel grade read a factor of 0 as "add the offset", so art lit to black kept most of
 *     its colour);
 *   - "shading pixel" in hd.txt keeps the old per-pixel grade everywhere.
 *
 * Pack layout (a folder, given by DEVILUTIONX_HD_PACK):
 *   hd.txt                                    "scale N", then one "image <file> <palette file>" per line, and optionally
 *                                             "silhouette free" (see below)
 *   sprite/<mpq path without ext>/<group>.<frame>.png   a CEL/CL2 frame, any size (sampled proportionally)
 *   sprite/<mpq path without ext>@<swap>/<group>.<frame>.png   the frame in a palette swap of its own: a monster
 *                                             type's TRN ("@bluered"), a unique monster's on top ("@bluered+rcrn",
 *                                             or "@+eth" on a type without one). Drawn as it is where the pack has
 *                                             it; otherwise the frame without the swap is graded as usual.
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
 */

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "engine/clx_sprite.hpp"
#include "engine/point.hpp"
#include "engine/surface.hpp"

struct SDL_Surface;
struct SDL_Renderer;

namespace devilution::hd {

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
		p[i] = { TrackImage, TrackLeft, TrackTop, src[i], dst[i], TrackFree, TrackUnder, TrackShade, light, TrackSerial };
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
		p[i] = { TrackImage, TrackLeft, TrackTop, src[i], dst[i], TrackFree, TrackUnder, TrackShade, static_cast<uint8_t>(lights[i] & 15), TrackSerial };
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
		p[i] = { TrackImage, TrackLeft, TrackTop, color, dst[i], TrackFree, TrackUnder, TrackShade, 0, TrackSerial };
	}
}

/** Whether an HD pack is loaded. */
bool Enabled();

/** Loads the pack named by DEVILUTIONX_HD_PACK, if any. */
void Init();

/** The pack's scale ("scale N" in its hd.txt), read without loading it; 1 when there is no pack. For sizing the window before Init. */
int PackScale();

/** Remembers which file, group and frame each sprite of a freshly loaded CEL/CL2 file is. ``path`` may carry an extension. */
void RegisterSprites(std::string_view path, ClxSpriteListOrSheet sprites);
void RegisterSpriteList(std::string_view path, uint16_t group, ClxSpriteList list);

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

/** Names a TRN that sprites are drawn through (a unique monster's: "rcrn"), for "@<swap>+<name>" art. */
void NameTrn(const uint8_t *trn, std::string_view name);

/**
 * Off-screen 8-bit buffers the game composes once and then copies to the screen (the control panel's BottomBuffer, the flask buffers): draws
 * into a registered buffer are tracked in a plane of its own, and Surface blits from it into the PalSurface carry that provenance along, so the
 * panel's art gets its HD image too. Blits from a buffer the layer does not track clear the provenance they cover. Forget a buffer before it is
 * freed.
 */
void TrackOffscreen(SDL_Surface *buffer);
void ForgetOffscreen(SDL_Surface *buffer);

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

/** The level tileset whose pillars DrawCell draws: "town", "hftown", "l1".."l6". */
void SetTileset(std::string_view name);

/**
 * The game's light tables (``size`` bytes from ``tables``): a sprite drawn through one of them is only lit, and takes one grade for the whole
 * draw; through any other TRN it is recoloured, and is graded locally (see the top of this file).
 */
void SetLightTables(const uint8_t *tables, size_t size);

/**
 * Tracks the pixels a CLX sprite draw writes (when it draws into the PalSurface). ``drawTrn``: the TRN the draw maps through. ``local``: the
 * draw's pixels don't share one light (blended over what is behind, or lit by the per-pixel lightmap), so they are graded locally.
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

/** Tracks the pixels of one pillar: ``bottomLeft`` is the bottom-left pixel of its lowest pieces. */
class PillarScope {
public:
	PillarScope(const Surface &out, Point bottomLeft, uint16_t pillar, int rows);
	~PillarScope();
};

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
