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
 * Pack layout (a folder, given by DEVILUTIONX_HD_PACK):
 *   hd.txt                                    "scale N", then one "image <file> <palette file>" per line
 *   sprite/<mpq path without ext>/<group>.<frame>.png   a CEL/CL2 frame, any size (sampled proportionally)
 *   sprite/<mpq path without ext>@<swap>/<group>.<frame>.png   the frame in a palette swap of its own: a monster
 *                                             type's TRN ("@bluered"), a unique monster's on top ("@bluered+rcrn",
 *                                             or "@+eth" on a type without one). Drawn as it is where the pack has
 *                                             it; otherwise the frame without the swap is graded as usual.
 *   tile/<tileset>/<pillar>.png               a level pillar (.min entry): 64 x 32*rows original pixels
 *   palettes/<mpq path>.pal                   768-byte RGB reference palettes
 */

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
};

/** Set while a tracked draw into the PalSurface runs: the plane, and the PalSurface pixel it starts at. */
extern PixelProvenance *TrackPlane;
extern const uint8_t *TrackBase;
extern uint32_t TrackImage;
extern int16_t TrackLeft;
extern int16_t TrackTop;

/** Record ``n`` pixels just written at ``dst`` from the art indices ``src``. */
inline void Mark(const uint8_t *dst, const uint8_t *src, unsigned n)
{
	if (TrackPlane == nullptr) return;
	PixelProvenance *p = TrackPlane + (dst - TrackBase);
	for (unsigned i = 0; i < n; ++i)
		p[i] = { TrackImage, TrackLeft, TrackTop, src[i], dst[i] };
}

/** Record ``n`` pixels just written at ``dst`` from one art index. */
inline void MarkFill(const uint8_t *dst, uint8_t color, unsigned n)
{
	if (TrackPlane == nullptr) return;
	PixelProvenance *p = TrackPlane + (dst - TrackBase);
	for (unsigned i = 0; i < n; ++i)
		p[i] = { TrackImage, TrackLeft, TrackTop, color, dst[i] };
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

/** The 8-bit surface the game renders into (the one to track). */
void SetTarget(SDL_Surface *palSurface);

/** The level tileset whose pillars DrawCell draws: "town", "hftown", "l1".."l6". */
void SetTileset(std::string_view name);

/** Tracks the pixels a CLX sprite draw writes (when it draws into the PalSurface). ``drawTrn``: the TRN the draw maps through. */
class SpriteScope {
public:
	SpriteScope(const Surface &out, Point position, ClxSprite clx, const uint8_t *drawTrn = nullptr);
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

/**
 * Composes the HD frame from ``palSurface`` and draws it with ``renderer`` (when not null).
 * Returns false when the HD layer is off, so the caller presents as usual.
 */
bool Present(SDL_Renderer *renderer, SDL_Surface *palSurface);

} // namespace devilution::hd
