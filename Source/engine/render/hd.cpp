#include "engine/render/hd.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif

#include "engine/render/hd_archive.hpp"
#include "utils/file_util.h"
#include "utils/log.hpp"
#include "utils/png.h"

namespace devilution::hd {

thread_local PixelProvenance *TrackPlane = nullptr;
thread_local const uint8_t *TrackBase = nullptr;
thread_local uint32_t TrackImage = 0;
thread_local int16_t TrackLeft = 0;
thread_local int16_t TrackTop = 0;
thread_local uint8_t TrackFree = 0;
thread_local uint32_t TrackSerial = 0;
thread_local uint8_t TrackUnder = 0;
thread_local uint8_t TrackShade = 0;
thread_local bool TrackBlend = false;

namespace {

#if !defined(USE_SDL1) && !defined(USE_SDL3)
constexpr bool Supported = true;
#else
constexpr bool Supported = false;
#endif

struct HdImage {
	int w = 0; ///< HD size
	int h = 0;
	int loW = 0; ///< the original's size, which the HD art covers
	int loH = 0;
	std::vector<uint32_t> px;     ///< 0xAARRGGBB, for art of more than 256 colours (empty otherwise: see idx)
	std::vector<uint8_t> idx;     ///< art of 256 colours or fewer: per pixel, where its colour is in ``colours`` (StorePixels; read with Pixel)
	std::vector<uint32_t> colours; ///< 0xAARRGGBB
	std::array<uint8_t, 768> ref; ///< the palette the art was authored against
	bool hasRef = false;
	uint32_t palette = 0; ///< which reference palette (PaletteKeys): pillars are graded by light level and palette
	bool world = false;   ///< art of the game world (monsters, heroes, towners, items, objects, missiles, levels), not of the panels and menus
	bool figure = false;  ///< a monster's, hero's or towner's art (OverEarlierFigure)
	bool monster = false; ///< a monster's art (MinBright)
	std::vector<uint8_t> reach; ///< per original pixel: the art is opaque somewhere in its block (free silhouettes; made when first needed)
	std::vector<uint8_t> propSource; ///< a prop's: per original pixel, the reference palette's colour nearest its art there, 0 where it has none (PropSource)
	uint32_t fx = 0;            ///< a sprite frame's effect layer ("fx/..."), an image of its own; looked up when the frame is first drawn into the game view
	bool fxLooked = false;
	int fxDx = 0; ///< an effect layer: where its rectangle (loW x loH original pixels) starts, relative to its sprite's top-left
	int fxDy = 0;
	bool boxed = false; ///< a sprite frame whose art covers more than the sprite ("hdbox"): loW x loH is then the art's rectangle
	int boxDx = 0;      ///< where that rectangle starts, relative to the sprite's top-left
	int boxDy = 0;
	std::string file;   ///< the pack file the picture was loaded from: a palette swap's art finds its own effect layer by it (FxOf)
	bool resident = false; ///< its pixels are loaded: ImageId loads them, Collect lets go of them (the rest stays, so the id stays the file's)
	bool pending = false;  ///< in PendingRelease
	uint32_t refs = 0;     ///< what holds it: registered sprite frames that drew it (SpriteEntry::images), the tileset (TileHeld), the art whose effect layer it is
	uint32_t lastUse = 0;  ///< UseClock when it was last drawn
	uint32_t letGoView = 0;    ///< ViewClock and UseClock when the last holder let go of it (Collect waits a little: GraceViews)
	uint32_t letGoPresent = 0;
	size_t bytes = 0;      ///< what its pixels and reach take (ResidentBytes)
};

struct SpriteEntry {
	std::string stem;  ///< "sprite/<path>"
	std::string frame; ///< "/<group>.<frame>.png"
	uint32_t check = 0;
	std::string recolour;          ///< the named TRN ClxApplyTrans applied ("bluered"), if any
	bool swapArt = false;          ///< the pack has this frame's "@<recolour>" art, which already shows it
	bool pending = false;          ///< between BeforeRecolour and AfterRecolour
	std::vector<uint8_t> original; ///< pixel data before ClxApplyTrans, when its own art is graded through it
	std::array<uint8_t, 256> trn;
	mutable const void *uniqueFor = nullptr; ///< the last unique item's art (UniqueItemArt) looked for, and what it found (ItemGraphic scopes)
	mutable uint32_t uniqueGeneration = 0;
	mutable uint32_t uniqueImage = 0;
	const void *owner = nullptr;  ///< the data the game frees the sprite with (RegisterSpriteList, ForgetSprites)
	std::vector<uint32_t> images; ///< the images its draws used, which it holds (HdImage::refs)
	const void *heldFor = nullptr; ///< the held item (HeldChoice) last looked for while a HeldItem lived, for the hero heldWho
	const void *heldWho = nullptr;
	uint32_t heldGeneration = 0;
	uint32_t heldImage = 0; ///< that item's held art for this frame (0: none), which it holds until the hero holds another (LetGoOfHeld)

	/** The pack file of this frame in a swap ("bluered", "bluered+rcrn", "+eth", "+general~moonbender"), or itself for none. */
	[[nodiscard]] std::string Key(std::string_view swap) const
	{
		return swap.empty() ? stem + frame : stem + "@" + std::string(swap) + frame;
	}
};

struct NamedTrn {
	std::string name;
	std::string own; ///< the name with its one monster's after a "~" ("general~moonbender"); empty when the TRN was named without one
	uint32_t check;
};

bool Active = false;
std::string PackDir;   ///< DEVILUTIONX_HD_PACK: a folder, or one archive (OpenPack)
bool LooseFiles = true; ///< the pack is a folder: its loose files are read, and win over its archives
/** The pack's archives (*.hdpk in its folder, in name order, or the one it is): a later one's entries replace an earlier one's. */
std::vector<std::unique_ptr<Archive>> Archives;
std::string ArchivesOf; ///< the pack the archives were opened for (OpenPack keeps them while they are unchanged)
int Scale = 4;
std::unordered_map<std::string, std::string> Manifest; ///< image file -> palette file
std::unordered_map<std::string, int> MonsterLights;   ///< "monsters/<path>[@<trn>]" -> light radius ("light" lines of hd.txt)
std::vector<HdImage> Images(1);                         ///< id 0: none; reserved for the whole manifest when the pack loads, so it never
                                                        ///< reallocates while Compose reads it (the level loader thread adds images)
/**
 * The main and the level loader thread look up, add and load images, register, recolour and forget sprites: Images (but the pixels of the
 * images the PalSurface shows, which only the main thread composes, draws into and lets go of: Collect), ImageIds, Sprites, Owners, TrnNames,
 * TileHeld and PendingRelease are used under it.
 */
std::mutex ImagesMutex;
std::unordered_map<std::string, uint32_t> ImageIds;     ///< image file -> id (0: no image)
std::unordered_map<const uint8_t *, SpriteEntry> Sprites;
std::unordered_map<const void *, std::vector<const uint8_t *>> Owners; ///< RegisterSpriteList's owner -> its sprites with art (ForgetSprites)
std::unordered_set<uint32_t> TileHeld;     ///< the pillars' images of the tileset in use, which it holds (SetTileset lets go of them)
std::vector<uint32_t> PendingRelease;      ///< images nothing holds any more: Collect lets go of their pixels once nothing on screen shows them
bool UnloadArt = true;                     ///< DEVILUTIONX_HD_UNLOAD=0: images are kept once loaded, as before
bool IndexedArt = true;                    ///< DEVILUTIONX_HD_INDEXED=0: every image kept in full colour, as before
size_t BudgetBytes = 0;                    ///< DEVILUTIONX_HD_MEMORY_MB / "memory <MB>": what held but idle images may take (0: no bound)
constexpr size_t DefaultBudgetMb = 4096;
constexpr uint32_t IdlePresents = 600;     ///< how long an image must go undrawn before the budget lets go of it
std::atomic<uint32_t> UseClock { 0 };      ///< counts presents: HdImage::lastUse
/**
 * Counts the game view's draws (ForgetView). An image nothing holds is let go of only once the view has been drawn GraceViews times since (or,
 * without a game view, after GracePresents presents): the game frees sprites and loads the same ones again (a hero's graphics on every level
 * change, a monster type that is on the next level too), and the new sprites hold the image again when they first draw. The loading screen
 * draws no view, so what was on the last level waits for the next one's first frames.
 */
std::atomic<uint32_t> ViewClock { 0 };
constexpr uint32_t GraceViews = 8;
constexpr uint32_t GracePresents = 600;
uint32_t LastBudgetScan = 0;
std::atomic<size_t> ResidentBytes { 0 };
std::atomic<size_t> PeakBytes { 0 };
std::atomic<uint32_t> LoadCount { 0 };
std::atomic<uint32_t> UnloadCount { 0 };
std::string MemLog;                        ///< DEVILUTIONX_HD_MEMLOG
double ComposeMs = 0;                      ///< time spent composing, for MemLog
std::atomic<uint64_t> LoadMicros { 0 };    ///< time spent loading images (LoadImage), for MemLog
std::atomic<uint32_t> LoadMaxMicros { 0 }; ///< the longest single image load, for MemLog
std::unordered_set<std::string> ArtStems; ///< "sprite/<path>" with art for any frame, its own or a swap's
std::string RecolourName;                 ///< the TRN ClxApplyTrans is applying, while a NamedRecolour lives
std::unordered_map<const uint8_t *, NamedTrn> TrnNames;
std::string Tileset;
std::unordered_map<std::string, int> TilePeriods; ///< hd.txt "tileperiod <tileset> <P>": pillars may have art per position modulo P
std::unordered_map<std::string, int> TileHashes;  ///< hd.txt "tilehash <tileset> <K>": pillars may have K variants picked by position
/** Props (hd.hpp, "Props"): hd.txt "propfloor <tileset> <pillar>|<a>-<b> ...", by tileset. */
std::unordered_map<std::string, std::vector<std::pair<int, int>>> PropFloorLines;
/** The props of the tileset in use, by file (SetTileset reads them; read-only while the level draws). */
std::unordered_map<std::string, Prop> Props;
constexpr uint8_t PropHas = 1;      ///< PropFlags: the pillar has a prop
constexpr uint8_t PropClears = 2;   ///< its pieces above its floor are not drawn
constexpr uint8_t PropVariants = 4; ///< it has props for some positions
std::vector<uint8_t> PropFlags;     ///< by pillar
std::vector<const Prop *> PropOwn;  ///< by pillar: its own prop (no position's), if any
std::vector<Displacement> PropFeetNow;
bool PropsNow = false;
std::string PropsTileset; ///< what PropFlags were made for ("<tileset>/<rows>"); cleared by Init
int TilesetRows = 0;      ///< SetTileset's rows, for Init to read the props again of a tileset set before the pack loaded
void ReadProps(std::string_view name, int rows);
thread_local std::string ItemArt; ///< "c057" while an ItemGraphic lives: the dropped item's own art is looked for first

/** One unique item's art: the pack file name it is under, worked out when the item is first drawn (and again if its table entry changed). */
struct UniqueItemArt {
	int cursor = -1;
	std::string source;  ///< the table name it was worked out from
	std::string name;    ///< "c057~the-butcher-s-cleaver"; empty when the pack has no art of this unique's own
};
std::unordered_set<std::string> ItemUniqueNames; ///< the "c<NNN>~<slug>" names the pack has art under
std::deque<UniqueItemArt> ItemUniqueArts;        ///< by unique item index (stable addresses)
uint32_t ItemUniqueGeneration = 1;               ///< changes whenever one of them is worked out again
thread_local const UniqueItemArt *ItemUnique = nullptr; ///< the unique item's art while an ItemGraphic for it lives

/** One sprite file's frames drawn holding one item ("plrgfx/warrior/wls/wlsas" in a held folder's render.json). */
struct HeldKit {
	uint8_t dirs = 0;    ///< the groups (a hero's directions) it has frames of, a bit each
	uint16_t frames = 0; ///< frames in each
};
/** A folder of hero renders: "held <dir>" (one weapon each) or "pairs <dir>" (a weapon and a shield each, or a shield alone). */
struct HeroArtFolder {
	std::string prefix; ///< what the pack files of its frames start with: "held/", "pairs/"
	std::string dir;    ///< <dir>/<key>/render.json and its frames; empty: none
	std::string rel;    ///< the same folder in the pack's archives ("held"); empty when the hd.txt line (or variable) gave an absolute path
	std::unordered_map<std::string, std::unordered_map<std::string, HeldKit>> kits; ///< "c057", "c057~doombringer", "c057+c083" -> sprite path -> its kit
	uint32_t kitCount = 0;
};
HeroArtFolder HeldFolder { "held/", {}, {}, {}, 0 };  ///< DEVILUTIONX_HD_HELD
HeroArtFolder PairFolder { "pairs/", {}, {}, {}, 0 }; ///< DEVILUTIONX_HD_PAIRS
std::string HeldPalette;     ///< the reference palette of held art whose weapon set's frame the pack lacks: that of the pack's hero art
uint32_t HeldGeneration = 1; ///< changes whenever a pack is loaded
size_t HeldImageCount = 0;   ///< images of held art: not in the manifest, so Images is kept that much larger than the manifest (ImageId)
/** What one hero holds: the folders its art may be in, most specific first ("pairs/c057+c083", "held/c057~doombringer", "held/c057"). */
struct HeldChoice {
	std::vector<std::string> folders;
};
std::unordered_map<std::string, HeldChoice> HeldChoices; ///< by the folders joined with "|", made once each (stable addresses)
/** What a hero (HeldItem's ``who``) holds, and its frames that hold held art for it. */
struct HeroHeld {
	const HeldChoice *choice = nullptr;
	std::vector<const uint8_t *> frames; ///< Sprites keys
};
std::unordered_map<const void *, HeroHeld> HeldBy;
thread_local const HeldChoice *HeldNow = nullptr; ///< while a HeldItem with held art lives
thread_local HeroHeld *HeldHero = nullptr;
thread_local const void *HeldWho = nullptr;
std::atomic<HeroArt> HeroArtLevel { HeroArt::UniqueWeaponsAndShields }; ///< SetHeroArt; changed under ImagesMutex

/** Whether ``file`` is a frame of a hero render folder ("held/...", "pairs/..."). */
bool HeroArtFile(const std::string &file)
{
	return file.compare(0, 5, "held/") == 0 || file.compare(0, 6, "pairs/") == 0;
}

/** "shading pixel" in hd.txt: every pixel graded on its own, as before the shading was reworked (see the top of hd.hpp). */
bool PixelShading = false;
/** PixelProvenance::shade */
constexpr uint8_t ShadeDraw = 0;
constexpr uint8_t ShadeLocal = 1;
constexpr uint8_t ShadePixel = 2;
constexpr uint8_t ShadeLevel = ShadeByLevel;
/** How far a local grade looks for pixels of the same draw, in original pixels each way. */
constexpr int LocalRadius = 2;
/** SetLightTables */
const uint8_t *LightTablesBegin = nullptr;
const uint8_t *LightTablesEnd = nullptr;
/** SetCycledColors: first | last << 8 (the game may set it from another thread than the one presenting). */
std::atomic<uint16_t> CycledRange { 0x0001 };
/** The indices of CycledRange as the frame being composed sees them (Compose sets it before its threads start). */
std::array<bool, 256> Cycled {};

/** Whether a pixel drawn from the art index ``source`` as ``index`` shows a colour the level animates (SetCycledColors). */
bool IsCycled(uint8_t source, uint8_t index)
{
	return Cycled[source] || Cycled[index];
}

/**
 * A minimum brightness for monsters in dim light (off unless the pack asks for it, and then only monsters' art is touched): the hd.txt line
 * "minbright a 0.20", or DEVILUTIONX_HD_MINBRIGHT="a:0.20" (the variable wins). After the grade of a monster's pixel, each channel is no darker than
 * the art's own (unlit) channel times the strength (0..1), so the art keeps its contrast and its colours, and a monster with dark art stays darker
 * than one with pale art. ("a" names the rule; anything else is off.)
 * The strength fades out with the light level the draw was lit with: full up to level 11, then 3/4, 1/2, 1/4, and none at 15 (the game draws it black),
 * so a monster at the edge of the light stays as black as it is. The game itself hides monsters on unlit tiles (IsTileLit), so nothing new is ever drawn.
 * A draw through a TRN that isn't a light table (Infravision's red, Stone Curse's grey, a unique's colours) gets no floor: it would bring the art's
 * own colours back into the recolour.
 */
float MinBright = 0;
std::unordered_map<uint32_t, uint8_t> DrawLights; ///< by draw serial, how much of MinBright applies (0..255) to the monsters' draws of this frame

uint8_t FloorWeightOfLevel(uint8_t level)
{
	if (level <= 11) return 255;
	return level >= 15 ? 0 : static_cast<uint8_t>(255 - (level - 11) * 64);
}
/** Reference palette file -> HdImage::palette (under ImagesMutex), and how many there are. */
std::unordered_map<std::string, uint32_t> PaletteKeys;
std::atomic<uint32_t> PaletteCount { 0 };

SDL_Surface *Target = nullptr; ///< the PalSurface
std::vector<PixelProvenance> Plane;
int PlanePitch = 0;
int PlaneHeight = 0;

/**
 * Free silhouettes: under a sprite draw whose art is free, the pixels it may touch are copied here before it draws, so
 * that art which is transparent over a pixel the sprite drew can show what was behind (one layer deep).
 */
bool FreeSilhouette = false;
std::vector<PixelProvenance> BehindProv;
std::vector<uint8_t> BehindIdx;
/**
 * See-through draws (hd.hpp, "See-through"): for each pixel drawn blended (PixelProvenance::blend), the index its art was lit to, what the same
 * draw would have written opaque. What was behind it is in BehindProv / BehindIdx, as for free silhouettes. Made at the first blended draw.
 */
std::vector<uint8_t> BlendLit;
uint32_t NextSerial = 0;

/** ForgetView: the game view's rectangle, and the last draw before it was drawn anew. */
int ViewX0 = 0, ViewY0 = 0, ViewX1 = 0, ViewY1 = 0;
uint32_t SceneSerial = 0;

/** Whether ``p``, at (``x``, ``y``) of the PalSurface, was drawn before the game view it is in was drawn anew (ForgetView). */
bool Stale(const PixelProvenance &p, int x, int y)
{
	return x >= ViewX0 && x < ViewX1 && y >= ViewY0 && y < ViewY1 && static_cast<int32_t>(p.serial - SceneSerial) <= 0;
}

/** The effect layer of a sprite drawn into the game view since NewScene: composed over the frame (ComposeOverlays). */
struct Overlay {
	uint32_t fx;
	int left, top;      ///< where the layer's rectangle starts, in PalSurface coordinates
	int x0, y0, x1, y1; ///< the part of it inside the surface the sprite was drawn into
	uint32_t serial;    ///< the sprite's draw: pixels drawn later cover the layer
	std::vector<uint8_t> seen; ///< the PalSurface in x0..x1, y0..y1 as the sprite's draw left it: art without HD leaves no other trace of drawing later
};
std::vector<Overlay> Overlays;

std::vector<uint32_t> Frame;
SDL_Texture *FrameTexture = nullptr;
int TextureW = 0;
int TextureH = 0;

std::string DumpDir;
int DumpEvery = 0;
bool DumpIds = false; ///< also dump which image each pixel came from (ids-*.png, and ids.txt: id and file)
int PresentCount = 0;
int LastDumpStep = -1;
std::string RequestedDump; ///< a frame to dump under this name at the next present, whatever DumpEvery says

uint32_t Checksum(ClxSprite clx)
{
	uint32_t h = 2166136261U;
	auto mix = [&h](uint32_t v) { h = (h ^ v) * 16777619U; };
	mix(clx.width());
	mix(clx.height());
	mix(clx.pixelDataSize());
	const uint8_t *p = clx.pixelData();
	for (uint32_t i = 0; i < std::min<uint32_t>(64, clx.pixelDataSize()); ++i)
		mix(p[i]);
	return h;
}

uint32_t TrnChecksum(const uint8_t *trn)
{
	uint32_t h = 2166136261U;
	for (int i = 0; i < 256; ++i)
		h = (h ^ trn[i]) * 16777619U;
	return h;
}

/** "Monsters\Zombie\Bluered" -> "bluered": a TRN's name in pack paths. */
std::string TrnStem(std::string_view name)
{
	const size_t slash = name.find_last_of("\\/");
	std::string out(slash == std::string_view::npos ? name : name.substr(slash + 1));
	for (char &c : out)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return out;
}

/** "Bonesaw the Litch" -> "bonesaw-the-litch": a unique monster's name in pack paths (letters and digits, anything else a single "-"). */
std::string NameSlug(std::string_view name)
{
	std::string out;
	bool gap = false;
	for (const char c : name) {
		if (std::isalnum(static_cast<unsigned char>(c)) == 0) {
			gap = !out.empty();
			continue;
		}
		if (gap) out += '-';
		gap = false;
		out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}

/** The names of a TRN that draws pass, when it was named and hasn't changed since. */
const NamedTrn *TrnNamed(const uint8_t *trn)
{
	if (trn == nullptr) return nullptr;
	const auto found = TrnNames.find(trn);
	if (found == TrnNames.end() || found->second.check != TrnChecksum(trn)) return nullptr;
	return &found->second;
}

/** "Monsters\Zombie\ZombieW.CL2" -> "monsters/zombie/zombiew" */
std::string NormalizePath(std::string_view path)
{
	std::string out(path);
	for (char &c : out)
		c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	const size_t slash = out.rfind('/');
	const size_t dot = out.rfind('.');
	if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
		out.resize(dot);
	return out;
}

/** The art's pixel ``i`` (0xAARRGGBB), however it is kept. */
uint32_t Pixel(const HdImage &img, size_t i)
{
	return img.idx.empty() ? img.px[i] : img.colours[img.idx[i]];
}

/**
 * Keeps the pixels of the ARGB surface ``argb`` in ``img``: as 8-bit indices into a palette of the art's own colours when it has 256 or fewer (the
 * pack's art is pixel-filtered to fewer), a quarter of the memory; else in full colour. Every colour is kept exactly, but a fully transparent pixel
 * of a sprite's or a pillar's art is kept as 0: nothing ever shows its colour (SampleBlock blends it away entirely, ComputeReach reads alpha alone).
 * An effect layer's are kept as they are (``keepClear``): its right half adds its colour whatever the alpha.
 */
void StorePixels(const SDL_Surface *argb, bool keepClear, HdImage &img)
{
	const auto w = static_cast<size_t>(img.w), h = static_cast<size_t>(img.h);
	const auto rowOf = [argb](size_t y) { return reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(argb->pixels) + y * argb->pitch); };
	const auto fold = [keepClear](uint32_t c) { return keepClear || (c >> 24) != 0 ? c : 0U; };
	img.px.clear();
	img.idx.clear();
	img.colours.clear();
	if (IndexedArt) {
		constexpr size_t Slots = 1024; // open addressing over the colours seen: 256 fill a quarter of it at most
		std::array<uint32_t, Slots> keys;
		std::array<int16_t, Slots> slots;
		slots.fill(-1);
		img.idx.resize(w * h);
		bool fits = true;
		uint32_t last = 0;
		int16_t lastSlot = -1;
		for (size_t y = 0; y < h && fits; ++y) {
			const uint32_t *row = rowOf(y);
			uint8_t *out = &img.idx[y * w];
			for (size_t x = 0; x < w; ++x) {
				const uint32_t c = fold(row[x]);
				if (lastSlot >= 0 && c == last) {
					out[x] = static_cast<uint8_t>(lastSlot);
					continue;
				}
				size_t s = static_cast<uint32_t>(c * 2654435761U) >> 22;
				while (slots[s] >= 0 && keys[s] != c)
					s = (s + 1) & (Slots - 1);
				if (slots[s] < 0) {
					if (img.colours.size() == 256) {
						fits = false;
						break;
					}
					keys[s] = c;
					slots[s] = static_cast<int16_t>(img.colours.size());
					img.colours.push_back(c);
				}
				last = c;
				lastSlot = slots[s];
				out[x] = static_cast<uint8_t>(lastSlot);
			}
		}
		if (fits) {
			img.colours.shrink_to_fit();
			return;
		}
		std::vector<uint8_t>().swap(img.idx);
		std::vector<uint32_t>().swap(img.colours);
	}
	img.px.resize(w * h);
	for (size_t y = 0; y < h; ++y) {
		const uint32_t *row = rowOf(y);
		for (size_t x = 0; x < w; ++x)
			img.px[y * w + x] = fold(row[x]);
	}
}

/** ``n`` more bytes of ``img`` loaded. */
void AddBytes(HdImage &img, size_t n)
{
	img.bytes += n;
	const size_t now = ResidentBytes += n;
	for (size_t peak = PeakBytes; now > peak && !PeakBytes.compare_exchange_weak(peak, now);) { }
}

/** Where the pack file ``file`` is on disk: held art ("held/<key>/...") and pair art ("pairs/<key>/...") in their folders, the rest in the pack. */
std::string PathOf(const std::string &file)
{
	for (const HeroArtFolder *folder : { &HeldFolder, &PairFolder })
		if (!folder->dir.empty() && file.compare(0, folder->prefix.size(), folder->prefix) == 0) return folder->dir + file.substr(folder->prefix.size() - 1);
	return PackDir + "/" + file;
}

bool IsAbsolutePath(std::string_view path)
{
	return !path.empty() && (path[0] == '/' || path[0] == '\\' || (path.size() > 1 && path[1] == ':'));
}

/** The name of the pack file ``file`` in the pack's archives (held and pair art under their folder's), or nothing: it is only ever loose. */
std::string ArchiveName(const std::string &file)
{
	for (const HeroArtFolder *folder : { &HeldFolder, &PairFolder }) {
		if (folder->dir.empty() || file.compare(0, folder->prefix.size(), folder->prefix) != 0) continue;
		return folder->rel.empty() ? std::string() : folder->rel + file.substr(folder->prefix.size() - 1);
	}
	return file;
}

/** Whether the pack file ``file`` may be a loose file (the pack is a folder, or it is in a folder given by an absolute path). */
bool LooseAllowed(const std::string &file)
{
	return LooseFiles || ArchiveName(file).empty();
}

/** The entry of the pack file ``file`` in the latest archive that names it; false when none does, or that one removed it. */
bool ArchiveEntry(const std::string &file, const Archive *&archive, Archive::Entry &entry)
{
	if (Archives.empty()) return false;
	const std::string name = ArchiveName(file);
	if (name.empty()) return false;
	for (auto it = Archives.rbegin(); it != Archives.rend(); ++it) {
		if (!(*it)->Find(name, entry)) continue;
		if (entry.codec == Archive::Removed) return false;
		archive = it->get();
		return true;
	}
	return false;
}

/** The bytes of the pack file ``file`` (hd.txt, a palette, a render.json): the loose file's when there is one, else the archives'. */
bool ReadPackFile(const std::string &file, std::vector<uint8_t> &out)
{
	if (LooseAllowed(file)) {
		std::ifstream in(PathOf(file), std::ios::binary);
		if (in) {
			out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			return true;
		}
	}
	const Archive *archive = nullptr;
	Archive::Entry entry;
	return ArchiveEntry(file, archive, entry) && archive->Read(entry, out);
}

/**
 * Sets PackDir and opens the pack's archives. DEVILUTIONX_HD_PACK names a folder (its loose files, and its *.hdpk archives in name order
 * under them) or one archive (nothing loose). Archives open for the same pack are kept while they are unchanged on disk (PackScale, Init).
 */
void OpenPack(const std::string &pack)
{
	std::vector<std::string> files;
	LooseFiles = DirectoryExists(pack);
	PackDir = pack;
	if (LooseFiles) {
		while (PackDir.size() > 1 && (PackDir.back() == '/' || PackDir.back() == '\\'))
			PackDir.pop_back();
		for (const std::string &name : ListFiles(PackDir.c_str())) {
			if (name.size() > 5 && name.compare(name.size() - 5, 5, ".hdpk") == 0 && name[0] != '.') files.push_back(PackDir + "/" + name);
		}
		std::sort(files.begin(), files.end());
	} else {
		files.push_back(pack);
	}
	bool same = ArchivesOf == pack && Archives.size() == files.size();
	for (size_t i = 0; same && i < files.size(); ++i)
		same = Archives[i]->Path() == files[i] && Archives[i]->Unchanged();
	if (same) return;
	Archives.clear();
	ArchivesOf = pack;
	for (const std::string &file : files) {
		auto archive = std::make_unique<Archive>();
		if (!archive->Open(file)) continue;
		LogInfo("HD: archive {} ({} entries)", file, archive->Size());
		Archives.push_back(std::move(archive));
	}
}

/** An image's pixels from an Img8 archive entry, kept as StorePixels keeps a surface's. */
void StoreImg8(const Img8Image &pic, bool keepClear, HdImage &img)
{
	img.w = static_cast<int>(pic.width);
	img.h = static_cast<int>(pic.height);
	img.px.clear();
	img.idx.clear();
	img.colours.clear();
	std::vector<uint32_t> colours(pic.colourCount);
	for (size_t i = 0; i < colours.size(); ++i) {
		const uint32_t c = pic.Colour(i);
		colours[i] = keepClear || (c >> 24) != 0 ? c : 0U;
	}
	const size_t n = static_cast<size_t>(pic.width) * pic.height;
	if (IndexedArt) {
		img.colours = std::move(colours);
		img.idx.assign(pic.indices, pic.indices + n);
		return;
	}
	img.px.resize(n);
	for (size_t i = 0; i < n; ++i)
		img.px[i] = colours[pic.indices[i]];
}

/**
 * Whether ``file`` is one of the pack's images, and the reference palette it was made against: the manifest's, or held (or pair) art that
 * its folder's render.json lists ("held/<key>/plrgfx/<path>/<group>.<frame>.png"), with the palette of the weapon set's frame it stands in for.
 */
bool PackImage(const std::string &file, std::string &palette)
{
	if (const auto entry = Manifest.find(file); entry != Manifest.end()) {
		palette = entry->second;
		return true;
	}
	const HeroArtFolder *folder = nullptr;
	for (const HeroArtFolder *f : { &HeldFolder, &PairFolder })
		if (!f->kits.empty() && file.compare(0, f->prefix.size(), f->prefix) == 0) folder = f;
	if (folder == nullptr) return false;
	const size_t keyAt = folder->prefix.size();
	const size_t keyEnd = file.find('/', keyAt);
	const size_t frameAt = file.rfind('/');
	if (keyEnd == std::string::npos || frameAt <= keyEnd) return false;
	const auto kits = folder->kits.find(file.substr(keyAt, keyEnd - keyAt));
	if (kits == folder->kits.end()) return false;
	const auto kit = kits->second.find(file.substr(keyEnd + 1, frameAt - keyEnd - 1));
	if (kit == kits->second.end()) return false;
	unsigned group = 0, frame = 0;
	char tail[8] = {};
	if (std::sscanf(file.c_str() + frameAt + 1, "%u.%u.%4s", &group, &frame, tail) != 3 || std::strcmp(tail, "png") != 0) return false;
	if (group >= 8 || (kit->second.dirs & (1U << group)) == 0 || frame >= kit->second.frames) return false;
	const auto generic = Manifest.find("sprite/" + file.substr(keyEnd + 1));
	palette = generic != Manifest.end() ? generic->second : HeldPalette;
	return true;
}

/** A PNG from a file (``path``) or from memory (``data``). */
SDL_Surface *LoadPng(const std::string &path, const std::vector<uint8_t> *data)
{
	// open and close the file here: IMG_LoadPNG(path) leaves its file open, one descriptor per HD image, until the process
	// runs out of them (Peter, 2026-11-01: crashed in the dungeon with "Too many open files")
#ifdef USE_SDL3
	SDL_IOStream *src = data != nullptr ? SDL_IOFromConstMem(data->data(), data->size()) : SDL_IOFromFile(path.c_str(), "rb");
	SDL_Surface *loaded = src != nullptr ? IMG_LoadPNG_IO(src) : nullptr;
	if (src != nullptr) SDL_CloseIO(src);
#else
	SDL_RWops *src = data != nullptr ? SDL_RWFromConstMem(data->data(), static_cast<int>(data->size())) : SDL_RWFromFile(path.c_str(), "rb");
	SDL_Surface *loaded = src != nullptr ? IMG_LoadPNG_RW(src) : nullptr;
	if (src != nullptr) SDL_RWclose(src);
#endif
	return loaded;
}

/** Adds the time from its making to its end to LoadMicros (and LoadMaxMicros). */
struct LoadTimer {
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	~LoadTimer()
	{
		const auto us = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
		LoadMicros += us;
		for (uint32_t most = LoadMaxMicros; us > most && !LoadMaxMicros.compare_exchange_weak(most, us);) { }
	}
};

/** Loads the pixels of the pack image ``file``: its loose file when there is one, else its archive entry. */
bool LoadImage(const std::string &file, const std::string &palette, HdImage &img)
{
	const LoadTimer timer;
	const bool keepClear = file.compare(0, 3, "fx/") == 0;
	const std::string path = PathOf(file);
	SDL_Surface *loaded = nullptr;
	if (LooseAllowed(file) && FileExists(path)) {
		loaded = LoadPng(path, nullptr);
		if (loaded == nullptr) {
			LogError("HD: can't load {}: {}", path, SDL_GetError());
			return false;
		}
	} else {
		const Archive *archive = nullptr;
		Archive::Entry entry;
		std::vector<uint8_t> bytes;
		if (!ArchiveEntry(file, archive, entry)) {
			LogError("HD: can't load {}: the pack has no such file", file);
			return false;
		}
		if (!archive->Read(entry, bytes)) return false;
		if (entry.codec == Archive::Img8) {
			Img8Image pic;
			if (!ParseImg8(bytes, pic)) {
				LogError("HD: can't load {} from {}: malformed", file, archive->Path());
				return false;
			}
			StoreImg8(pic, keepClear, img);
		} else {
			loaded = LoadPng(path, &bytes);
			if (loaded == nullptr) {
				LogError("HD: can't load {} from {}: {}", file, archive->Path(), SDL_GetError());
				return false;
			}
		}
	}
	if (loaded != nullptr) {
		SDL_Surface *argb = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_ARGB8888, 0);
		SDL_FreeSurface(loaded);
		if (argb == nullptr) return false;
		img.w = argb->w;
		img.h = argb->h;
		StorePixels(argb, keepClear, img);
		SDL_FreeSurface(argb);
	}
	if (!palette.empty()) {
		std::vector<uint8_t> pal;
		img.hasRef = ReadPackFile(palette, pal) && pal.size() >= img.ref.size();
		if (img.hasRef) std::copy_n(pal.begin(), img.ref.size(), img.ref.begin());
	}
	img.reach.clear();
	img.propSource.clear();
	img.bytes = 0;
	AddBytes(img, img.px.size() * sizeof(uint32_t) + img.idx.size() + img.colours.size() * sizeof(uint32_t));
	img.resident = true;
	++LoadCount;
	return true;
}

/** Lets go of an image's pixels (and reach): only on the main thread, while nothing on screen shows it (Collect). */
void Unload(HdImage &img)
{
	if (!img.resident) return;
	ResidentBytes -= img.bytes;
	img.bytes = 0;
	std::vector<uint32_t>().swap(img.px);
	std::vector<uint8_t>().swap(img.idx);
	std::vector<uint32_t>().swap(img.colours);
	std::vector<uint8_t>().swap(img.reach);
	std::vector<uint8_t>().swap(img.propSource);
	img.resident = false;
	++UnloadCount;
}

/** One more holder of the image ``id``. */
void Hold(uint32_t id)
{
	if (id != 0) ++Images[id].refs;
}

/** One holder fewer: an image nothing holds goes to PendingRelease. */
void LetGo(uint32_t id)
{
	if (id == 0 || Images[id].refs == 0) return;
	HdImage &img = Images[id];
	if (--img.refs != 0) return;
	img.letGoView = ViewClock.load(std::memory_order_relaxed);
	img.letGoPresent = UseClock.load(std::memory_order_relaxed);
	if (!img.pending) {
		img.pending = true;
		PendingRelease.push_back(id);
	}
}


std::string ReadPngText(const std::string &path, std::string_view keyword);

/** The tEXt chunk ``keyword`` of the pack image ``file``: its loose file's, else its archive entry's (kept in the archive's index). */
std::string PackText(const std::string &file, std::string_view keyword)
{
	if (LooseAllowed(file)) {
		const std::string path = PathOf(file);
		if (FileExists(path)) return ReadPngText(path, keyword);
	}
	const Archive *archive = nullptr;
	Archive::Entry entry;
	return ArchiveEntry(file, archive, entry) ? Archive::Text(entry, keyword) : std::string();
}

/**
 * A sprite frame's art may cover more than the sprite (a raised sword's tip above it, a foot below): its tEXt chunk "hdbox" =
 * "<dx> <dy> <w> <h>" is then the rectangle the picture covers, in original pixels relative to the sprite's top-left.
 */
void ReadBox(const std::string &file, HdImage &img)
{
	const std::string text = PackText(file, "hdbox");
	if (text.empty()) return;
	constexpr int Most = 4096;
	int dx = 0, dy = 0, w = 0, h = 0;
	std::istringstream words(text);
	if (!(words >> dx >> dy >> w >> h) || w <= 0 || h <= 0 || w > Most || h > Most || std::abs(dx) > Most || std::abs(dy) > Most) {
		LogError("HD: {} doesn't say what it covers (tEXt hdbox \"{}\")", file, text);
		return;
	}
	img.boxed = true;
	img.boxDx = dx;
	img.boxDy = dy;
	img.loW = w;
	img.loH = h;
}

/** The pixels of an image that were let go, loaded again (the file may have changed since; what it covers stays as first read). */
bool Reload(uint32_t id)
{
	HdImage &img = Images[id];
	std::string palette;
	if (PackImage(img.file, palette) && LoadImage(img.file, palette, img)) return true;
	img.w = img.h = 0; // drawn as art without HD
	return false;
}

/** The image of the pack file ``file``, its pixels loaded; 0 when the pack has none. Under ImagesMutex. */
uint32_t ImageId(const std::string &file, int loW, int loH)
{
	const auto found = ImageIds.find(file);
	if (found != ImageIds.end()) {
		const uint32_t known = found->second;
		if (known != 0 && !Images[known].resident && !Reload(known)) return 0;
		if (known != 0) Images[known].lastUse = UseClock.load(std::memory_order_relaxed);
		return known;
	}
	uint32_t id = 0;
	std::string palette;
	if (PackImage(file, palette)) {
		HdImage img;
		if (LoadImage(file, palette, img)) {
			img.loW = loW;
			img.loH = loH;
			img.file = file;
			const auto key = PaletteKeys.emplace(palette, static_cast<uint32_t>(PaletteKeys.size()));
			img.palette = key.first->second;
			PaletteCount = static_cast<uint32_t>(PaletteKeys.size());
			const bool held = HeroArtFile(file); // a hero's art (held or pair)
			for (const char *world : { "sprite/monsters/", "sprite/plrgfx/", "sprite/towners/", "sprite/items/", "sprite/objects/", "sprite/missiles/", "sprite/levels/", "sprite/nlevels/", "tile/", "prop/" })
				img.world = img.world || held || file.rfind(world, 0) == 0;
			for (const char *figure : { "sprite/monsters/", "sprite/plrgfx/", "sprite/towners/" })
				img.figure = img.figure || held || file.rfind(figure, 0) == 0;
			img.monster = file.rfind("sprite/monsters/", 0) == 0;
			if (file.compare(0, 7, "sprite/") == 0 || held) ReadBox(file, img);
			img.lastUse = UseClock.load(std::memory_order_relaxed);
			if (held) {
				// Images is reserved for the manifest so that it never reallocates while Compose reads it: the level loader thread adds the
				// manifest's images while the main thread composes the loading screen. Held art is added only by the main thread drawing a hero
				// in the game view (never while a level loads), so it grows Images here, keeping room for the whole manifest besides
				++HeldImageCount;
				const size_t need = Manifest.size() + 1 + HeldImageCount;
				if (Images.capacity() < need) Images.reserve(need + 4096);
			}
			Images.push_back(std::move(img));
			id = static_cast<uint32_t>(Images.size() - 1);
			if (DumpIds) std::ofstream(DumpDir + "/ids.txt", std::ios::app) << id << ' ' << file << '\n';
		}
	}
	ImageIds.emplace(file, id);
	return id;
}

/** The text of the PNG's tEXt chunk ``keyword`` (ahead of the pixel data), or nothing. */
std::string ReadPngText(const std::string &path, std::string_view keyword)
{
	std::ifstream in(path, std::ios::binary);
	char head[8];
	if (!in.read(head, 8)) return {};
	while (in.read(head, 8)) {
		const auto *b = reinterpret_cast<const uint8_t *>(head);
		const uint32_t size = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) | (static_cast<uint32_t>(b[2]) << 8) | b[3];
		const std::string_view type(head + 4, 4);
		if (type == "IDAT" || type == "IEND") break;
		if (type != "tEXt" || size > 4096) {
			in.seekg(static_cast<std::streamoff>(size) + 4, std::ios::cur);
			continue;
		}
		std::string data(size, '\0');
		if (!in.read(data.data(), size)) break;
		in.seekg(4, std::ios::cur);
		if (data.size() > keyword.size() && data.compare(0, keyword.size(), keyword) == 0 && data[keyword.size()] == '\0')
			return data.substr(keyword.size() + 1);
	}
	return {};
}

/**
 * The effect layer of the sprite frame ``entry``, drawn with the art ``image``: "fx/<path>/<group>.<frame>.png" beside
 * "sprite/<path>/<group>.<frame>.png". Art of a palette swap ("sprite/<path>@<swap>/...") may have a layer of its own, "fx/<path>@<swap>/...",
 * which then replaces the body's; a swap without one shows the body's. 0 when the pack has none, or the file doesn't say where it goes.
 */
uint32_t FxOf(uint32_t image, const SpriteEntry &entry)
{
	if (Images[image].fxLooked) {
		const uint32_t fx = Images[image].fx;
		if (fx == 0) return 0;
		if (!Images[fx].resident && !Reload(fx)) return 0; // let go of by the budget while its art was held
		Images[fx].lastUse = UseClock.load(std::memory_order_relaxed);
		return fx;
	}
	Images[image].fxLooked = true;
	std::string file = "fx" + entry.Key({}).substr(std::strlen("sprite"));
	const std::string artFile = Images[image].file; // (a copy: ImageId below may grow Images)
	if (artFile.compare(0, 7, "sprite/") == 0 && artFile != entry.Key({})) {
		const std::string swapFile = "fx" + artFile.substr(std::strlen("sprite"));
		if (Manifest.count(swapFile) != 0) file = swapFile;
	}
	if (Manifest.count(file) == 0) return 0;
	int dx = 0, dy = 0, w = 0, h = 0;
	std::istringstream words(PackText(file, "hdfx"));
	if (!(words >> dx >> dy >> w >> h) || w <= 0 || h <= 0) {
		LogError("HD: {} doesn't say where it goes (tEXt hdfx)", file);
		return 0;
	}
	const uint32_t fx = ImageId(file, w, h);
	if (fx == 0 || Images[fx].w < 2) return 0;
	Images[fx].fxDx = dx;
	Images[fx].fxDy = dy;
	Images[image].fx = fx;
	Hold(fx); // until the art it lights is let go of
	return fx;
}

/** The sprite frame ``entry`` drew with the image ``id``: it holds it until the game frees the sprite. */
void HoldFor(SpriteEntry &entry, uint32_t id)
{
	if (id == 0 || std::find(entry.images.begin(), entry.images.end(), id) != entry.images.end()) return;
	entry.images.push_back(id);
	Hold(id);
}

/** The sprite frame ``entry`` lets go of the held art it holds (its hero holds another item, or the game frees the frame). */
void LetGoOfHeld(SpriteEntry &entry)
{
	LetGo(entry.heldImage);
	entry.heldImage = 0;
	entry.heldFor = nullptr;
	entry.heldWho = nullptr;
}

/** Every frame of the hero ``who`` lets go of the held art it holds. Under ImagesMutex. */
void LetGoOfHeld(const void *who, HeroHeld &hero)
{
	for (const uint8_t *key : hero.frames) {
		const auto found = Sprites.find(key);
		if (found != Sprites.end() && found->second.heldWho == who) LetGoOfHeld(found->second);
	}
	hero.frames.clear();
	hero.choice = nullptr;
}

/** The sprite frame ``entry`` is forgotten: lets go of the images it held. */
void LetGoOf(SpriteEntry &entry)
{
	for (const uint32_t id : entry.images)
		LetGo(id);
	entry.images.clear();
	LetGoOfHeld(entry);
}

bool EnsurePlane()
{
	if (Target == nullptr) return false;
	if (PlanePitch != Target->pitch || PlaneHeight != Target->h) {
		PlanePitch = Target->pitch;
		PlaneHeight = Target->h;
		Plane.assign(static_cast<size_t>(PlanePitch) * PlaneHeight, PixelProvenance {});
		Overlays.clear();
		if (FreeSilhouette || !BehindProv.empty()) {
			BehindProv.assign(Plane.size(), PixelProvenance {});
			BehindIdx.assign(Plane.size(), 0);
		}
		if (!BlendLit.empty()) BlendLit.assign(Plane.size(), 0);
	}
	return true;
}

/** Whether the pixel ``p`` at ``at`` of the plane was drawn blended, with what was behind it kept (BlendLit, BehindProv / BehindIdx). */
bool Blended(const PixelProvenance &p, size_t at)
{
	return p.blend != 0 && BlendLit.size() == Plane.size() && BehindProv.size() == Plane.size() && at < BlendLit.size();
}

/** The index the art of the pixel ``p`` at ``at`` of the plane shows (``index``: the pixel's index now): for a blended pixel, its lit index. */
uint8_t ShownIndex(const PixelProvenance &p, size_t at, uint8_t index)
{
	return p.blend != 0 && at < BlendLit.size() ? BlendLit[at] : index;
}

struct SavedTrack {
	PixelProvenance *plane;
	const uint8_t *base;
	uint32_t image;
	int16_t left, top;
	uint8_t free;
	uint8_t shade;
	bool blend;
	uint32_t serial;
};
thread_local SavedTrack Saved;

/** The draw in progress, when its art has a free silhouette: where it may draw (the art's rectangle, clipped to the surface). */
struct FreeDraw {
	bool on = false;
	uint32_t image = 0;
	int left = 0, top = 0;
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	bool whole = false; ///< no limit on how far from the sprite's own pixels its art is drawn (art with an effect layer, or a box of its own)
	uint32_t serial = 0; ///< the draw's serial: the pixels it wrote carry it
};
thread_local FreeDraw Free;

/** What was under the art's rectangle (the PalSurface's indices and the pixels' provenance) when a free draw began: BehindIdx / BehindProv take it only where the draw wrote a pixel (CommitBehind). */
thread_local std::vector<uint8_t> SavedIdx;
thread_local std::vector<PixelProvenance> SavedProv;

/** The sprite draw in progress, when its art has an effect layer: End adds it to Overlays. */
struct FxDraw {
	uint32_t fx = 0;
	int left = 0, top = 0;              ///< the sprite's top-left (its art's may be elsewhere: "hdbox")
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0; ///< the surface the sprite is drawn into
};
thread_local FxDraw Fx;

/** A tracked off-screen buffer (TrackOffscreen) and its provenance plane, indexed like its pixels (row pitch). */
struct Offscreen {
	SDL_Surface *surface;
	std::vector<PixelProvenance> plane;
};
std::vector<Offscreen> Offscreens;

Offscreen *FindOffscreen(const SDL_Surface *surface)
{
	for (Offscreen &o : Offscreens)
		if (o.surface == surface) return &o;
	return nullptr;
}

/** SurfaceBlitHook: a copy into the PalSurface takes the provenance of what it copies (none, from an untracked source). */
void CopyProvenance(const Surface &src, SDL_Rect r, const Surface &dst, Point at, bool skipZero)
{
	if (!Active || dst.surface != Target || src.surface == nullptr || !EnsurePlane()) return;
	const Offscreen *off = FindOffscreen(src.surface);
	const bool self = src.surface == Target;
	const int sx = src.region.x + r.x, sy = src.region.y + r.y;
	const int dx = dst.region.x + at.x, dy = dst.region.y + at.y;
	const auto *pixels = static_cast<const uint8_t *>(src.surface->pixels);
	const int srcPitch = src.surface->pitch;
	std::vector<PixelProvenance> row;
	const uint32_t serial = ++NextSerial; // a copy is a draw of its own: it covers the effect layers of the sprites drawn before it
	for (int y = 0; y < r.h; ++y) {
		const int ty = dy + (self && dy > sy ? r.h - 1 - y : y), fy = sy + (self && dy > sy ? r.h - 1 - y : y);
		PixelProvenance *to = &Plane[static_cast<size_t>(ty) * PlanePitch + dx];
		const uint8_t *idx = pixels + static_cast<size_t>(fy) * srcPitch + sx;
		if (self) row.assign(&Plane[static_cast<size_t>(fy) * PlanePitch + sx], &Plane[static_cast<size_t>(fy) * PlanePitch + sx] + r.w);
		for (int x = 0; x < r.w; ++x) {
			if (skipZero && idx[x] == 0) continue;
			if (off == nullptr && !self) {
				to[x] = {};
				to[x].serial = serial;
				continue;
			}
			PixelProvenance p = self ? row[x] : off->plane[static_cast<size_t>(fy) * srcPitch + sx + x];
			if (self && (Stale(p, sx + x, fy) || p.blend != 0)) p = {}; // (what was behind a blended pixel stays where it was)
			if (p.image != 0) {
				p.left = static_cast<int16_t>(p.left + dx - sx);
				p.top = static_cast<int16_t>(p.top + dy - sy);
			}
			p.serial = serial;
			to[x] = p;
		}
	}
}

void Begin(const Surface &out, uint32_t image, int left, int top, bool free = false)
{
	Saved = { TrackPlane, TrackBase, TrackImage, TrackLeft, TrackTop, TrackFree, TrackShade, TrackBlend, TrackSerial };
	TrackShade = PixelShading ? ShadePixel : ShadeDraw;
	TrackBlend = false;
	Free.on = false;
	Fx.fx = 0;
	if (!Active) return;
	if (out.surface != Target) {
		Offscreen *off = FindOffscreen(out.surface);
		if (off == nullptr) return;
		TrackPlane = off->plane.data(); // an off-screen buffer: tracked plainly (no free silhouettes)
		TrackBase = static_cast<const uint8_t *>(out.surface->pixels);
		TrackImage = image;
		TrackLeft = static_cast<int16_t>(left);
		TrackTop = static_cast<int16_t>(top);
		TrackFree = 0;
		TrackSerial = ++NextSerial;
		return;
	}
	if (!EnsurePlane()) return;
	TrackPlane = Plane.data();
	TrackBase = static_cast<const uint8_t *>(Target->pixels);
	TrackImage = image;
	TrackLeft = static_cast<int16_t>(left);
	TrackTop = static_cast<int16_t>(top);
	TrackFree = 0;
	TrackSerial = ++NextSerial;
	TrackBlend = true;
	if (!free || !FreeSilhouette || image == 0 || Images[image].w == 0 || BehindProv.size() != Plane.size()) return;
	const HdImage &img = Images[image];
	const int x0 = std::max({ left, out.region.x, 0 });
	const int y0 = std::max({ top, out.region.y, 0 });
	const int x1 = std::min({ left + img.loW, out.region.x + out.region.w, Target->w });
	const int y1 = std::min({ top + img.loH, out.region.y + out.region.h, Target->h });
	if (x0 >= x1 || y0 >= y1) return;
	Free = { true, image, left, top, x0, y0, x1, y1 };
	Free.serial = TrackSerial;
	TrackFree = 1;
	const auto *pixels = static_cast<const uint8_t *>(Target->pixels);
	const size_t rowLen = static_cast<size_t>(x1 - x0);
	SavedIdx.resize(rowLen * (y1 - y0));
	SavedProv.resize(rowLen * (y1 - y0));
	for (int y = y0; y < y1; ++y) {
		const size_t at = static_cast<size_t>(y) * PlanePitch + x0;
		std::memcpy(&SavedIdx[(y - y0) * rowLen], pixels + at, rowLen);
		std::copy_n(&Plane[at], rowLen, &SavedProv[(y - y0) * rowLen]);
	}
}

/** Per original pixel: whether the art is opaque (alpha 32 or more) anywhere in the pixel's block. */
void ComputeReach(HdImage &img)
{
	const int n = Scale;
	img.reach.assign(static_cast<size_t>(img.loW) * img.loH, 0);
	for (int ly = 0; ly < img.loH; ++ly) {
		for (int lx = 0; lx < img.loW; ++lx) {
			uint32_t best = 0;
			for (int j = 0; j < n; ++j) {
				const int v = std::min(img.h - 1, (ly * n + j) * img.h / (img.loH * n));
				for (int i = 0; i < n; ++i) {
					const int u = std::min(img.w - 1, (lx * n + i) * img.w / (img.loW * n));
					best = std::max(best, Pixel(img, static_cast<size_t>(v) * img.w + u) >> 24);
				}
			}
			img.reach[static_cast<size_t>(ly) * img.loW + lx] = best >= 32 ? 1 : 0;
		}
	}
	AddBytes(img, img.reach.size());
}

/**
 * ExtendFootprint for art with an effect layer (a spell cast) or a box of its own ("hdbox"): no limit on how far from the sprite's own pixels
 * the art is drawn, since its pose is not bound to the original's. Two passes over the art's rectangle give each pixel its nearest drawn pixel by chessboard
 * distance (a near-black one counts as 3 further away, as below).
 */
void ExtendWholeArt(const HdImage &img)
{
	const int w = Free.x1 - Free.x0, h = Free.y1 - Free.y0;
	std::vector<int> dist(static_cast<size_t>(w) * h, 1 << 20);
	std::vector<int> donor(dist.size(), -1); // the drawn pixel a pixel takes after, as an index into the rectangle
	bool any = false;
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const PixelProvenance &q = Plane[static_cast<size_t>(Free.y0 + y) * PlanePitch + Free.x0 + x];
			if (q.serial != TrackSerial) continue;
			const uint8_t *ref = &img.ref[q.source * 3];
			const size_t i = static_cast<size_t>(y) * w + x;
			dist[i] = img.hasRef && ref[0] + ref[1] + ref[2] < 90 ? 3 : 0;
			donor[i] = static_cast<int>(i);
			any = true;
		}
	}
	if (!any) return; // the sprite drew nothing here (clipped away)
	const auto take = [&](size_t i, int qx, int qy) {
		if (qx < 0 || qx >= w || qy < 0 || qy >= h) return;
		const size_t q = static_cast<size_t>(qy) * w + qx;
		if (dist[q] + 1 < dist[i]) {
			dist[i] = dist[q] + 1;
			donor[i] = donor[q];
		}
	};
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const size_t i = static_cast<size_t>(y) * w + x;
			take(i, x - 1, y);
			take(i, x - 1, y - 1);
			take(i, x, y - 1);
			take(i, x + 1, y - 1);
		}
	}
	for (int y = h - 1; y >= 0; --y) {
		for (int x = w - 1; x >= 0; --x) {
			const size_t i = static_cast<size_t>(y) * w + x;
			take(i, x + 1, y);
			take(i, x + 1, y + 1);
			take(i, x, y + 1);
			take(i, x - 1, y + 1);
		}
	}
	struct Extension {
		size_t at;
		PixelProvenance donor;
	};
	std::vector<Extension> extend; // applied after the scan, as below
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const size_t at = static_cast<size_t>(Free.y0 + y) * PlanePitch + Free.x0 + x;
			const int d = donor[static_cast<size_t>(y) * w + x];
			if (d < 0 || Plane[at].serial == TrackSerial) continue;
			if (img.reach[static_cast<size_t>(Free.y0 + y - Free.top) * img.loW + (Free.x0 + x - Free.left)] == 0) continue;
			extend.push_back({ at, Plane[static_cast<size_t>(Free.y0 + d / w) * PlanePitch + Free.x0 + d % w] });
		}
	}
	auto *pixels = static_cast<uint8_t *>(Target->pixels);
	for (const Extension &e : extend) {
		pixels[e.at] = e.donor.written;
		Plane[e.at] = { Free.image, static_cast<int16_t>(Free.left), static_cast<int16_t>(Free.top), e.donor.source, e.donor.written, 2, 0, TrackShade, e.donor.light, 0, TrackSerial };
	}
}

/**
 * Whether the pixel at ``at`` still shows a figure (a monster, hero or towner) drawn before the figure being drawn now: one drawn earlier
 * stands further back, so the nearer figure's art goes over it as far as the art reaches (Peter 2026-11-15: "the monster is in front of
 * the player spatially, but the player clips the monster and obscures the monster's shield").
 */
bool OverEarlierFigure(size_t at)
{
	const PixelProvenance &q = Plane[at];
	return q.image != 0 && q.serial != TrackSerial && Images[q.image].figure && Images[Free.image].figure
	    && q.written == static_cast<const uint8_t *>(Target->pixels)[at];
}

/**
 * Draws the art's pixels outside the sprite's footprint into the PalSurface: each takes the index, and so the lighting,
 * of the nearest pixel the sprite drew (preferring one that isn't near black), and is recorded as the sprite's own (free == 2: foliage drawn
 * later goes under it, see TrackUnder).
 */
void ExtendFootprint()
{
	HdImage &img = Images[Free.image];
	if (img.reach.empty()) ComputeReach(img);
	if (Free.whole) {
		ExtendWholeArt(img);
		return;
	}
	auto *pixels = static_cast<uint8_t *>(Target->pixels);
	constexpr int Radius = 6;
	constexpr int FigureRadius = 24; // over a figure drawn earlier (OverEarlierFigure)
	struct Extension {
		size_t at;
		const PixelProvenance *donor;
	};
	std::vector<Extension> extend;
	for (int y = Free.y0; y < Free.y1; ++y) {
		for (int x = Free.x0; x < Free.x1; ++x) {
			const size_t at = static_cast<size_t>(y) * PlanePitch + x;
			if (Plane[at].serial == TrackSerial) continue; // the sprite drew it
			if (img.reach[static_cast<size_t>(y - Free.top) * img.loW + (x - Free.left)] == 0) continue;
			const PixelProvenance *best = nullptr;
			int bestScore = 1000;
			const int limit = OverEarlierFigure(at) ? FigureRadius : Radius;
			for (int r = 1; r <= limit && r < bestScore; ++r) {
				for (int dy = -r; dy <= r; ++dy) {
					for (int dx = -r; dx <= r; ++dx) {
						if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
						const int qx = x + dx, qy = y + dy;
						if (qx < Free.x0 || qx >= Free.x1 || qy < Free.y0 || qy >= Free.y1) continue;
						const PixelProvenance &q = Plane[static_cast<size_t>(qy) * PlanePitch + qx];
						if (q.serial != TrackSerial) continue;
						const uint8_t *ref = &img.ref[q.source * 3];
						const bool dark = img.hasRef && ref[0] + ref[1] + ref[2] < 90;
						const int score = r + (dark ? 3 : 0);
						if (score < bestScore) {
							best = &q;
							bestScore = score;
						}
					}
				}
			}
			if (best != nullptr) extend.push_back({ at, best });
		}
	}
	// Applied after the scan, so that pixels added here are not taken as donors in turn
	std::vector<PixelProvenance> donors;
	donors.reserve(extend.size());
	for (const Extension &e : extend)
		donors.push_back(*e.donor);
	for (size_t i = 0; i < extend.size(); ++i) {
		pixels[extend[i].at] = donors[i].written;
		Plane[extend[i].at] = { Free.image, static_cast<int16_t>(Free.left), static_cast<int16_t>(Free.top), donors[i].source, donors[i].written, 2, 0, TrackShade, donors[i].light, 0, TrackSerial };
	}
}

/** The sprite just drawn has an effect layer: composed over the frame from now on, where the sprite's surface lets it. */
void AddOverlay()
{
	const HdImage &img = Images[Fx.fx];
	Overlay o { Fx.fx, Fx.left + img.fxDx, Fx.top + img.fxDy, 0, 0, 0, 0, TrackSerial, {} };
	o.x0 = std::max(o.left, Fx.x0);
	o.y0 = std::max(o.top, Fx.y0);
	o.x1 = std::min(o.left + img.loW, Fx.x1);
	o.y1 = std::min(o.top + img.loH, Fx.y1);
	if (o.x0 >= o.x1 || o.y0 >= o.y1) return;
	const int w = o.x1 - o.x0;
	o.seen.resize(static_cast<size_t>(w) * (o.y1 - o.y0));
	for (int y = o.y0; y < o.y1; ++y)
		std::memcpy(&o.seen[static_cast<size_t>(y - o.y0) * w], static_cast<const uint8_t *>(Target->pixels) + static_cast<size_t>(y) * Target->pitch + o.x0, w);
	for (Overlay &earlier : Overlays) {
		if (earlier.fx == o.fx && earlier.left == o.left && earlier.top == o.top) { // the same sprite drawn again: one layer, at its later draw
			earlier = std::move(o);
			return;
		}
	}
	Overlays.push_back(std::move(o));
}

/**
 * The pixels a free draw wrote (its own, and the ones ExtendFootprint added) keep what was under them: what BehindIdx / BehindProv held there
 * is replaced. A pixel the draw did not write keeps the record of the draw that did: the art's rectangle also covers the transparent parts
 * of a sprite, and recording those too made a later sprite whose rectangle overlaps an earlier one (a town building over the hero) take the
 * earlier sprite's pixels for the "ground" under them, so the earlier art blended over its own 1997 original where it is transparent.
 */
void CommitBehind()
{
	const size_t rowLen = static_cast<size_t>(Free.x1 - Free.x0);
	if (BehindProv.size() != Plane.size() || SavedIdx.size() != rowLen * (Free.y1 - Free.y0) || SavedProv.size() != SavedIdx.size()) return;
	for (int y = Free.y0; y < Free.y1; ++y)
		for (int x = Free.x0; x < Free.x1; ++x) {
			const size_t at = static_cast<size_t>(y) * PlanePitch + x;
			if (Plane[at].serial != Free.serial || Plane[at].blend != 0) continue; // (a blended pixel kept what was behind it as it drew: KeepBehind)
			const size_t saved = (y - Free.y0) * rowLen + (x - Free.x0);
			PixelProvenance behind = SavedProv[saved];
			uint8_t index = SavedIdx[saved];
			if (behind.blend != 0) { // a blended pixel behind: its art as drawn opaque (one layer deep), as KeepBehind keeps it
				if (behind.image != 0 && behind.written == index && at < BlendLit.size()) index = behind.written = BlendLit[at];
				behind.blend = 0;
			}
			BehindIdx[at] = index;
			BehindProv[at] = behind;
		}
}

void End()
{
	if (Free.on) {
		ExtendFootprint();
		CommitBehind();
		Free.on = false;
	}
	if (Fx.fx != 0) {
		AddOverlay();
		Fx.fx = 0;
	}
	TrackPlane = Saved.plane;
	TrackBase = Saved.base;
	TrackImage = Saved.image;
	TrackLeft = Saved.left;
	TrackTop = Saved.top;
	TrackFree = Saved.free;
	TrackShade = Saved.shade;
	TrackBlend = Saved.blend;
	TrackSerial = Saved.serial;
}

/**
 * How one channel of an HD pixel follows the game's picture: times factor (16.16 fixed point), plus offset, clamped. A light table steps each
 * colour down its ramp, which takes about the same brightness off every colour and stops at black (dim light crushes the shadows), so a
 * draw's grade is the line through its pixels' (reference, live) pairs above black: factor below 1 and a negative offset.
 */
struct ChannelGrade {
	int32_t factor = 1 << 16;
	int32_t offset = 0;

	ChannelGrade() = default;

	/**
	 * The per-pixel grade of "shading pixel", exactly as it always was: times live / ref (rounded up, so that ref itself maps exactly to
	 * live), or plus live - ref when ref is too dark for a ratio. That ratio's 0 meant "use the offset", so a pixel lit to black (live 0) took
	 * the offset -ref instead: dark monsters kept most of their art.
	 */
	ChannelGrade(uint8_t ref, uint8_t live)
	{
		const int32_t ratio = ref >= 16 ? static_cast<int32_t>(((live << 16) + ref - 1) / ref) : 0;
		if (ratio != 0)
			factor = ratio;
		else
			offset = live - ref;
	}

	/** Times ``scale``, plus ``shift``. */
	static ChannelGrade Line(double scale, double shift)
	{
		ChannelGrade g;
		g.factor = static_cast<int32_t>(std::lround(std::clamp(scale, 0.0, 8.0) * 65536.0));
		g.offset = static_cast<int32_t>(std::lround(std::clamp(shift, -255.0, 255.0)));
		return g;
	}

	/**
	 * Over ``n`` pixels: the sum of their live colours over the sum of their reference colours, or the mean offset when the references are too
	 * dark for a ratio. A sum lit to black grades to black.
	 */
	static ChannelGrade OfSums(uint64_t ref, uint64_t live, uint32_t n)
	{
		if (n == 0) return {};
		if (ref >= 16U * n) return Line(static_cast<double>(live) / static_cast<double>(ref), 0);
		return Line(1, (static_cast<double>(live) - static_cast<double>(ref)) / n);
	}

	[[nodiscard]] uint32_t apply(uint32_t c) const
	{
		const int64_t v = ((static_cast<int64_t>(c) * factor) >> 16) + offset;
		return static_cast<uint32_t>(std::clamp<int64_t>(v, 0, 255));
	}
};

/** How an HD pixel's three channels follow the game's picture. */
struct Grade {
	ChannelGrade r, g, b;
};

/** Least-squares sums of (reference, live) pairs: a line through them. */
struct LineSums {
	uint64_t n = 0, x = 0, y = 0, xx = 0, xy = 0;

	void Add(uint32_t ref, uint32_t live, uint32_t count = 1)
	{
		n += count;
		x += static_cast<uint64_t>(ref) * count;
		y += static_cast<uint64_t>(live) * count;
		xx += static_cast<uint64_t>(ref) * ref * count;
		xy += static_cast<uint64_t>(ref) * live * count;
	}

	void Merge(const LineSums &o)
	{
		n += o.n;
		x += o.x;
		y += o.y;
		xx += o.xx;
		xy += o.xy;
	}

	/** The line, when there are enough pairs spread widely enough (references with a spread of 4 levels or more): its scale and shift. */
	[[nodiscard]] bool Fit(double &scale, double &shift) const
	{
		if (n < 6) return false;
		const auto nn = static_cast<int64_t>(n), sx = static_cast<int64_t>(x), sy = static_cast<int64_t>(y);
		const int64_t den = nn * static_cast<int64_t>(xx) - sx * sx;
		if (den < 16 * nn * nn) return false;
		scale = static_cast<double>(nn * static_cast<int64_t>(xy) - sx * sy) / static_cast<double>(den);
		if (scale < 0) return false;
		shift = (static_cast<double>(sy) - scale * static_cast<double>(sx)) / static_cast<double>(nn);
		return true;
	}
};

/** The line a channel's pairs above black give (its own, else the grey ones'), else the ratio of all its pairs' sums. */
ChannelGrade LineOf(const LineSums *lit, int k, uint64_t ref, uint64_t live, uint32_t n)
{
	double scale, shift;
	if (lit[k].Fit(scale, shift)) return ChannelGrade::Line(scale, shift);
	if (lit[3].Fit(scale, shift)) return ChannelGrade::Line(scale, shift / 3);
	return ChannelGrade::OfSums(ref, live, n);
}

/**
 * A channel's line comes from its pairs above black only. Where the light takes many colours to black (deep shadow), the line leaves the art
 * brighter than the game draws it: shifts the line until the pairs' reference values come out as bright, in all, as the game drew them
 * (``live``, the sum of their live values; ``forEach(f)`` calls f(value, count) for the reference values). A line that already does (no
 * colour taken to black) is left as it is.
 */
template <typename ForEach>
void MatchBrightness(ChannelGrade &c, uint64_t live, uint32_t n, uint64_t aboveBlack, const ForEach &forEach)
{
	if (aboveBlack == n) return; // no pixel lit to black: the line goes through them all already
	const auto predicted = [&](double shift) {
		const double scale = c.factor / 65536.0, add = c.offset + shift;
		double total = 0;
		forEach([&](uint32_t value, uint32_t count) { total += count * std::clamp(value * scale + add, 0.0, 255.0); });
		return total;
	};
	const auto target = static_cast<double>(live);
	if (std::abs(predicted(0) - target) <= 0.01 * target + 0.5 * n) return;
	double lo = -255, hi = 255;
	for (int i = 0; i < 10; ++i) {
		const double mid = (lo + hi) / 2;
		(predicted(mid) < target ? lo : hi) = mid;
	}
	c.offset += static_cast<int32_t>(std::lround((lo + hi) / 2));
}

/**
 * The pixels of one draw, or of one light level of one reference palette (see the top of hd.hpp), by the art's colour index: how many there
 * are of each and what the game drew them as. Within one draw (one light table) and one light level the drawn colour is a function of the
 * art's, so counting is all a pixel costs; the grade is made from the 256 counts (ToGrade), not from every pixel.
 */
struct GradeSums {
	const uint8_t *palette = nullptr;     ///< the reference palette of the art (the first's: a draw's art shares one)
	std::array<uint32_t, 256> count {};   ///< how many pixels have each reference index
	std::array<uint8_t, 256> written {};  ///< the index the game drew them as (the last one seen)

	void Add(const uint8_t *refPalette, uint8_t source, uint8_t index)
	{
		if (palette == nullptr) palette = refPalette;
		++count[source];
		written[source] = index;
	}

	void Merge(const GradeSums &o)
	{
		if (palette == nullptr) palette = o.palette;
		for (int s = 0; s < 256; ++s) {
			if (o.count[s] == 0) continue;
			count[s] += o.count[s];
			written[s] = o.written[s];
		}
	}

	[[nodiscard]] Grade ToGrade(const SDL_Color *live) const
	{
		if (palette == nullptr) return {};
		LineSums lit[4];
		uint64_t liveSum[3] {}, refSum[3] {};
		uint64_t above[3] {};
		uint32_t n = 0;
		for (int s = 0; s < 256; ++s) {
			const uint32_t c = count[s];
			if (c == 0) continue;
			const SDL_Color l = live[written[s]];
			const uint8_t lv[3] = { l.r, l.g, l.b };
			const uint8_t *ref3 = &palette[s * 3];
			for (int k = 0; k < 3; ++k) {
				liveSum[k] += static_cast<uint64_t>(lv[k]) * c;
				refSum[k] += static_cast<uint64_t>(ref3[k]) * c;
				if (lv[k] != 0) {
					lit[k].Add(ref3[k], lv[k], c);
					above[k] += c;
				}
			}
			if (l.r + l.g + l.b != 0) lit[3].Add(ref3[0] + ref3[1] + ref3[2], l.r + l.g + l.b, c);
			n += c;
		}
		Grade g { LineOf(lit, 0, refSum[0], liveSum[0], n), LineOf(lit, 1, refSum[1], liveSum[1], n), LineOf(lit, 2, refSum[2], liveSum[2], n) };
		ChannelGrade *channels[3] = { &g.r, &g.g, &g.b };
		for (int k = 0; k < 3; ++k) {
			MatchBrightness(*channels[k], liveSum[k], n, above[k], [&](const auto &f) {
				for (int s = 0; s < 256; ++s)
					if (count[s] != 0) f(palette[s * 3 + k], count[s]);
			});
		}
		return g;
	}
};

/** The same for the few pixels round one pixel (a local grade): the pairs themselves. */
struct LocalSums {
	static constexpr int Most = (2 * LocalRadius + 1) * (2 * LocalRadius + 1);
	LineSums lit[4];
	uint64_t live[3] {};
	uint64_t ref[3] {};
	uint32_t n = 0;
	uint8_t refs[Most][3];

	void Add(SDL_Color c, const uint8_t *ref3)
	{
		const uint8_t l[3] = { c.r, c.g, c.b };
		for (int k = 0; k < 3; ++k) {
			live[k] += l[k];
			ref[k] += ref3[k];
			if (l[k] != 0) lit[k].Add(ref3[k], l[k]);
			refs[n][k] = ref3[k];
		}
		if (c.r + c.g + c.b != 0) lit[3].Add(ref3[0] + ref3[1] + ref3[2], c.r + c.g + c.b);
		++n;
	}

	[[nodiscard]] Grade ToGrade() const
	{
		Grade g { LineOf(lit, 0, ref[0], live[0], n), LineOf(lit, 1, ref[1], live[1], n), LineOf(lit, 2, ref[2], live[2], n) };
		ChannelGrade *channels[3] = { &g.r, &g.g, &g.b };
		for (int k = 0; k < 3; ++k) {
			MatchBrightness(*channels[k], live[k], n, lit[k].n, [&](const auto &f) {
				for (uint32_t i = 0; i < n; ++i)
					f(refs[i][k], 1);
			});
		}
		return g;
	}
};

/** The grade of each draw graded as a whole (shade 0), by its serial: made by PrepareGrades before the frame is composed. */
std::unordered_map<uint32_t, Grade> DrawGrades;
/** The grade of each light level of each reference palette (shade 3: pillars), at palette * 16 + light; absent where none was drawn. */
std::vector<Grade> LevelGrades;
std::vector<uint8_t> LevelGraded;

/** What one band of rows adds up (PrepareRows): by draw, and by palette and light level. */
struct BandSums {
	std::unordered_map<uint32_t, GradeSums> draws;
	std::vector<GradeSums> levels;
};

/** Whether ``p`` still holds what its draw wrote (``index``: the pixel's index now), with art that is graded. */
bool Graded(const PixelProvenance &p, uint8_t index)
{
	return p.image != 0 && p.written == index && Images[p.image].w != 0 && Images[p.image].hasRef;
}

/** PrepareGrades over the rows ``y0`` to ``y1``: the sums of each draw graded as a whole. */
/**
 * Before the rows ``y0`` to ``y1`` are composed: clears the pixels drawn before the game view was drawn anew (Stale), and, unless ``sums`` is
 * null, sums each draw graded as a whole (PrepareGrades).
 */
void PrepareRows(SDL_Surface *pal, int y0, int y1, BandSums *sums)
{
	for (int y = std::max(y0, ViewY0); y < std::min(y1, ViewY1); ++y) {
		for (int x = ViewX0; x < ViewX1; ++x) {
			PixelProvenance &p = Plane[static_cast<size_t>(y) * PlanePitch + x];
			if (p.image != 0 && Stale(p, x, y)) p = {};
		}
	}
	if (sums == nullptr) return;
	const auto *pixels = static_cast<const uint8_t *>(pal->pixels);
	const bool haveBehind = BehindProv.size() == Plane.size();
	uint32_t lastSerial = 0;
	GradeSums *last = nullptr;
	// ``shown``: the index the art is graded by, the pixel's own but for a blended one (its lit index: the pair its draw would have made opaque)
	const auto add = [&](const PixelProvenance &p, uint8_t index, uint8_t shown) {
		if ((p.shade != ShadeDraw && p.shade != ShadeLevel) || p.free == 2 || !Graded(p, index) || IsCycled(p.source, shown)) return;
		const HdImage &img = Images[p.image];
		if (p.shade == ShadeLevel) {
			const size_t at = static_cast<size_t>(img.palette) * 16 + p.light;
			if (at < sums->levels.size()) sums->levels[at].Add(img.ref.data(), p.source, shown);
			return;
		}
		if (last == nullptr || p.serial != lastSerial) {
			last = &sums->draws[p.serial];
			lastSerial = p.serial;
		}
		last->Add(img.ref.data(), p.source, shown);
	};
	for (int y = y0; y < y1; ++y) {
		for (int x = 0; x < pal->w; ++x) {
			const size_t at = static_cast<size_t>(y) * pal->pitch + x;
			const PixelProvenance &p = Plane[at];
			if (p.image == 0) continue;
			const bool blended = p.blend != 0 && Blended(p, at);
			add(p, pixels[at], blended ? BlendLit[at] : pixels[at]);
			// what shows behind a free silhouette, or half behind a blended pixel that still shows, is graded as its own draw
			if (haveBehind && (p.free != 0 || (blended && p.written == pixels[at])) && !Stale(BehindProv[at], x, y)) add(BehindProv[at], BehindIdx[at], BehindIdx[at]);
		}
	}
}

/**
 * Every draw graded as a whole: the sums of its pixels still on screen (and of those kept behind free silhouettes), the pixels its art
 * added outside its footprint left out, as they repeat others; ``sums`` holds them by bands of rows (PrepareRows).
 */
void PrepareGrades(std::vector<BandSums> &sums, const SDL_Color *live)
{
	for (size_t t = 1; t < sums.size(); ++t) {
		for (const auto &[serial, s] : sums[t].draws)
			sums[0].draws[serial].Merge(s);
		for (size_t l = 0; l < sums[0].levels.size(); ++l)
			sums[0].levels[l].Merge(sums[t].levels[l]);
	}
	DrawGrades.reserve(sums[0].draws.size());
	for (const auto &[serial, s] : sums[0].draws)
		DrawGrades.emplace(serial, s.ToGrade(live));
	LevelGrades.assign(sums[0].levels.size(), Grade {});
	LevelGraded.assign(sums[0].levels.size(), 0);
	for (size_t l = 0; l < sums[0].levels.size(); ++l) {
		if (sums[0].levels[l].palette == nullptr) continue;
		LevelGrades[l] = sums[0].levels[l].ToGrade(live);
		LevelGraded[l] = 1;
	}
}

/** A local grade (shade 1): the sums over the pixels of the same draw within LocalRadius of (``x``, ``y``). False when there are none. */
bool LocalGrade(const PixelProvenance *plane, const uint8_t *idx, int pitch, int w, int h, int x, int y, uint32_t serial, const SDL_Color *live, Grade &out)
{
	LocalSums s;
	for (int qy = std::max(0, y - LocalRadius); qy <= std::min(h - 1, y + LocalRadius); ++qy) {
		for (int qx = std::max(0, x - LocalRadius); qx <= std::min(w - 1, x + LocalRadius); ++qx) {
			const size_t at = static_cast<size_t>(qy) * pitch + qx;
			const PixelProvenance &q = plane[at];
			if (q.serial != serial || !Graded(q, idx[at])) continue;
			const uint8_t shown = plane == Plane.data() ? ShownIndex(q, at, idx[at]) : idx[at];
			if (IsCycled(q.source, shown)) continue;
			s.Add(live[shown], &Images[q.image].ref[q.source * 3]);
		}
	}
	if (s.n == 0) return false;
	out = s.ToGrade();
	return true;
}

/** The last draw whose grade GradeOf looked up. */
struct GradeCache {
	uint32_t serial = 0;
	const Grade *grade = nullptr;
};

/**
 * The grade of the pixel ``p`` at (``x``, ``y``) of ``plane`` (its indices ``idx``, ``pitch`` apart, ``w`` x ``h``), as its shade says; null
 * for art without a reference palette (drawn as it is). ``scratch`` holds a grade made for this pixel, ``cache`` the last draw's.
 */
const Grade *GradeOf(const PixelProvenance &p, const PixelProvenance *plane, const uint8_t *idx, int pitch, int w, int h, int x, int y,
    const SDL_Color *live, Grade &scratch, GradeCache &cache)
{
	const HdImage &img = Images[p.image];
	if (!img.hasRef) return nullptr;
	const size_t pos = static_cast<size_t>(y) * pitch + x;
	const uint8_t index = plane == Plane.data() ? ShownIndex(p, pos, idx[pos]) : idx[pos]; // (a blended pixel: graded by its lit index)
	// a colour the level animates follows the animation, not the light: graded on its own, as no grade of the rest fits it
	const bool cycled = IsCycled(p.source, index);
	if (!cycled && p.shade == ShadeDraw) {
		if (cache.grade != nullptr && cache.serial == p.serial) return cache.grade;
		const auto found = DrawGrades.find(p.serial);
		if (found != DrawGrades.end()) {
			cache = { p.serial, &found->second };
			return cache.grade;
		}
	} else if (!cycled && p.shade == ShadeLevel) {
		const size_t at = static_cast<size_t>(img.palette) * 16 + p.light;
		if (at < LevelGraded.size() && LevelGraded[at] != 0) return &LevelGrades[at];
	} else if (!cycled && p.shade == ShadeLocal && LocalGrade(plane, idx, pitch, w, h, x, y, p.serial, live, scratch)) {
		return &scratch;
	}
	const uint8_t *ref = &img.ref[p.source * 3];
	const SDL_Color c = live[index];
	if (p.shade == ShadePixel || cycled)
		scratch = { ChannelGrade(ref[0], c.r), ChannelGrade(ref[1], c.g), ChannelGrade(ref[2], c.b) };
	else // a draw with no grade of its own here (none of its pixels is left to sum): this pixel alone
		scratch = { ChannelGrade::OfSums(ref[0], c.r, 1), ChannelGrade::OfSums(ref[1], c.g, 1), ChannelGrade::OfSums(ref[2], c.b, 1) };
	return &scratch;
}

/** The minimum brightness (MinBright) on a graded monster pixel: each channel no darker than the art's (``ar``...) times ``factor``. */
void ApplyFloor(float factor, uint32_t ar, uint32_t ag, uint32_t ab, uint32_t &r, uint32_t &g, uint32_t &b)
{
	r = std::max(r, static_cast<uint32_t>(ar * factor + 0.5F));
	g = std::max(g, static_cast<uint32_t>(ag * factor + 0.5F));
	b = std::max(b, static_cast<uint32_t>(ab * factor + 0.5F));
}

/** The factor of the floor for the monster draw ``p`` is part of (0 for anything else, and always without MinBright); ``last`` caches the last draw. */
struct FloorCache {
	uint32_t serial = 0;
	float factor = 0;
};
float FloorFactorOf(const PixelProvenance &p, FloorCache &last)
{
	if (MinBright <= 0 || !Images[p.image].monster) return 0;
	if (last.serial != p.serial) {
		const auto found = DrawLights.find(p.serial);
		last = { p.serial, found != DrawLights.end() ? MinBright * (found->second / 255.0F) : 0 };
	}
	return last.factor;
}

void FillBlock(uint32_t *dst, int outW, int n, uint32_t color)
{
	for (int j = 0; j < n; ++j)
		std::fill_n(dst + static_cast<size_t>(j) * outW, n, color);
}

/**
 * Draws the HD block of the pixel (``x``, ``y``), live colour ``c``, from ``img`` into ``dst`` (n rows of n, ``outW`` apart), graded
 * by ``grade`` (GradeOf; null: as it is). Where the art is not opaque it blends over ``under`` (n x n, packed) or, without it, over ``c``.
 * False when the pixel is outside the art.
 */
bool SampleBlock(const HdImage &img, const PixelProvenance &p, int x, int y, SDL_Color c, const Grade *grade, uint32_t *dst, int outW, const uint32_t *under,
    float floorFactor = 0)
{
	const int n = Scale;
	const int sx = x - p.left;
	const int sy = y - p.top;
	if (sx < 0 || sy < 0 || sx >= img.loW || sy >= img.loH) return false;
	int us[8];
	for (int i = 0; i < n; ++i)
		us[i] = std::min(img.w - 1, (sx * n + i) * img.w / (img.loW * n));
	const bool indexed = !img.idx.empty();
	const uint32_t *colours = img.colours.data();
	for (int j = 0; j < n; ++j) {
		const int v = std::min(img.h - 1, (sy * n + j) * img.h / (img.loH * n));
		const size_t rowAt = static_cast<size_t>(v) * img.w;
		const uint32_t *row = indexed ? nullptr : &img.px[rowAt];
		const uint8_t *indices = indexed ? &img.idx[rowAt] : nullptr;
		uint32_t *out = dst + static_cast<size_t>(j) * outW;
		for (int i = 0; i < n; ++i) {
			const uint32_t s = indexed ? colours[indices[us[i]]] : row[us[i]];
			const uint32_t a = s >> 24;
			uint32_t r = (s >> 16) & 0xFF, g = (s >> 8) & 0xFF, b = s & 0xFF;
			if (grade != nullptr) {
				const uint32_t artR = r, artG = g, artB = b;
				r = grade->r.apply(r);
				g = grade->g.apply(g);
				b = grade->b.apply(b);
				if (floorFactor > 0) ApplyFloor(floorFactor, artR, artG, artB, r, g, b);
			}
			if (a < 255) {
				uint32_t br = c.r, bg = c.g, bb = c.b;
				if (under != nullptr) {
					const uint32_t u = under[j * n + i];
					br = (u >> 16) & 0xFF;
					bg = (u >> 8) & 0xFF;
					bb = u & 0xFF;
				}
				r = (r * a + br * (255 - a)) / 255;
				g = (g * a + bg * (255 - a)) / 255;
				b = (b * a + bb * (255 - a)) / 255;
			}
			out[i] = 0xFF000000U | (r << 16) | (g << 8) | b;
		}
	}
	return true;
}

void ComposeRows(SDL_Surface *pal, int y0, int y1)
{
	const int w = pal->w;
	const int n = Scale;
	const int outW = w * n;
	const SDL_Color *live = pal->format->palette->colors;
	const auto *pixels = static_cast<const uint8_t *>(pal->pixels);
	const bool havePlane = PlanePitch == pal->pitch && PlaneHeight == pal->h;
	const bool haveBehind = havePlane && BehindProv.size() == Plane.size();
	Grade scratch, behindScratch;
	GradeCache cache, behindCache;
	FloorCache floorCache, behindFloorCache;
	// The HD block of what was behind the pixel at ``at`` (BehindProv / BehindIdx), graded as its own draw, or its plain colour, into ``under``
	const auto behindBlock = [&](size_t at, int x, int y, uint32_t *under) {
		const uint8_t behind = BehindIdx[at];
		const SDL_Color bc = live[behind];
		const PixelProvenance &bp = BehindProv[at];
		const HdImage *behindImg = bp.image != 0 && bp.written == behind && !Stale(bp, x, y) ? &Images[bp.image] : nullptr;
		if (behindImg == nullptr || behindImg->w == 0
		    || !SampleBlock(*behindImg, bp, x, y, bc, GradeOf(bp, BehindProv.data(), BehindIdx.data(), pal->pitch, w, pal->h, x, y, live, behindScratch, behindCache), under, n, nullptr, FloorFactorOf(bp, behindFloorCache)))
			FillBlock(under, n, n, 0xFF000000U | (bc.r << 16) | (bc.g << 8) | bc.b);
	};
	for (int y = y0; y < y1; ++y) {
		for (int x = 0; x < w; ++x) {
			const size_t at = static_cast<size_t>(y) * pal->pitch + x;
			const uint8_t index = pixels[at];
			const SDL_Color c = live[index];
			const uint32_t flat = 0xFF000000U | (c.r << 16) | (c.g << 8) | c.b;
			uint32_t *dst = &Frame[static_cast<size_t>(y) * n * outW + static_cast<size_t>(x) * n];
			const PixelProvenance *p = havePlane ? &Plane[at] : nullptr; // (PrepareRows cleared the pixels drawn before the view was drawn anew)
			const HdImage *img = p != nullptr && p->image != 0 && p->written == index ? &Images[p->image] : nullptr;
			if (img == nullptr || img->w == 0) {
				FillBlock(dst, outW, n, flat);
				continue;
			}
			if (p->blend != 0 && Blended(*p, at)) {
				// See-through: the art, graded by the index it was lit to, half over what was behind it (paletteTransparencyLookup's rule, in
				// true colour); where the art has no pixel, the pixel's own colour, as the game blended it
				uint32_t under[64];
				behindBlock(at, x, y, under);
				const SDL_Color lc = live[BlendLit[at]];
				if (!SampleBlock(*img, *p, x, y, lc, GradeOf(*p, Plane.data(), pixels, pal->pitch, w, pal->h, x, y, live, scratch, cache), dst, outW, under, FloorFactorOf(*p, floorCache))) {
					FillBlock(dst, outW, n, flat);
					continue;
				}
				for (int j = 0; j < n; ++j) {
					uint32_t *row = dst + static_cast<size_t>(j) * outW;
					for (int i = 0; i < n; ++i) {
						const uint32_t a = row[i], b = under[j * n + i];
						row[i] = 0xFF000000U | ((a & b & 0xFFFFFFU) + (((a ^ b) & 0xFEFEFEU) >> 1)); // per channel (a + b) / 2, as BlendColors
					}
				}
				continue;
			}
			if (p->free != 0 && haveBehind) {
				// Free silhouette: the art blends over what was behind the sprite here, not over the sprite's own pixel
				uint32_t under[64];
				behindBlock(at, x, y, under);
				if (!SampleBlock(*img, *p, x, y, c, GradeOf(*p, Plane.data(), pixels, pal->pitch, w, pal->h, x, y, live, scratch, cache), dst, outW, under, FloorFactorOf(*p, floorCache)))
					FillBlock(dst, outW, n, flat);
				continue;
			}
			if (!SampleBlock(*img, *p, x, y, c, GradeOf(*p, Plane.data(), pixels, pal->pitch, w, pal->h, x, y, live, scratch, cache), dst, outW, nullptr, FloorFactorOf(*p, floorCache)))
				FillBlock(dst, outW, n, flat);
		}
	}
}

/** How far the palette is faded in, 0..256: the game fades by scaling the whole palette, and a palette that isn't faded has a full-bright entry. */
uint32_t FadeLevel(const SDL_Color *live)
{
	int top = 0;
	for (int i = 0; i < 256; ++i)
		top = std::max({ top, static_cast<int>(live[i].r), static_cast<int>(live[i].g), static_cast<int>(live[i].b) });
	return top >= 248 ? 256 : static_cast<uint32_t>(top) * 256 / 248;
}

/**
 * Draws the effect layers over the composed frame: the left half of each by its alpha, the right half added. A pixel takes its part only
 * while nothing was drawn there after the layer's sprite: no HD art with a later serial (foliage aside, as for the art a free silhouette
 * adds), and no art without HD, which shows as a pixel that no longer holds what it held when the sprite had drawn (a wall in front, a
 * missile, the automap's lines, the panels, the cursor). Art without HD that writes the very index that was there goes unnoticed.
 */
void ComposeOverlays(SDL_Surface *pal)
{
	if (PlanePitch != pal->pitch || PlaneHeight != pal->h) return;
	const uint32_t fade = FadeLevel(pal->format->palette->colors);
	if (fade == 0) return;
	const int n = Scale;
	const int outW = pal->w * n;
	const auto *pixels = static_cast<const uint8_t *>(pal->pixels);
	for (const Overlay &o : Overlays) {
		const HdImage &img = Images[o.fx];
		const int half = img.w / 2;
		for (int y = std::max(o.y0, 0); y < std::min(o.y1, pal->h); ++y) {
			const int sy = y - o.top;
			for (int x = std::max(o.x0, 0); x < std::min(o.x1, pal->w); ++x) {
				const size_t at = static_cast<size_t>(y) * pal->pitch + x;
				const PixelProvenance &p = Plane[at];
				if (p.image != 0 && p.written == pixels[at] && static_cast<int32_t>(p.serial - o.serial) > 0) {
					if (p.under == 0) continue;
				} else if (pixels[at] != o.seen[static_cast<size_t>(y - o.y0) * (o.x1 - o.x0) + (x - o.x0)]) {
					continue;
				}
				const int sx = x - o.left;
				int us[8];
				for (int i = 0; i < n; ++i)
					us[i] = std::min(half - 1, (sx * n + i) * half / (img.loW * n));
				for (int j = 0; j < n; ++j) {
					const int v = std::min(img.h - 1, (sy * n + j) * img.h / (img.loH * n));
					const size_t rowAt = static_cast<size_t>(v) * img.w;
					uint32_t *out = &Frame[(static_cast<size_t>(y) * n + j) * outW + static_cast<size_t>(x) * n];
					for (int i = 0; i < n; ++i) {
						const uint32_t over = Pixel(img, rowAt + us[i]);
						const uint32_t glow = Pixel(img, rowAt + half + us[i]) & 0xFFFFFFU;
						const uint32_t a = over >> 24;
						if (a == 0 && glow == 0) continue;
						uint32_t c[3] = { (out[i] >> 16) & 0xFF, (out[i] >> 8) & 0xFF, out[i] & 0xFF };
						for (int k = 0; k < 3; ++k) {
							const int shift = 16 - 8 * k;
							c[k] = (c[k] * (255 - a) + ((((over >> shift) & 0xFF) * fade) >> 8) * a) / 255;
							c[k] = std::min<uint32_t>(255, c[k] + ((((glow >> shift) & 0xFF) * fade) >> 8));
						}
						out[i] = 0xFF000000U | (c[0] << 16) | (c[1] << 8) | c[2];
					}
				}
			}
		}
	}
}

/**
 * Before a frame is composed (main thread): lets go of the pixels of the images nothing holds any more (PendingRelease), and, above the
 * budget, of the images held but not drawn for longest. Neither while a pixel of the PalSurface, of what free silhouettes keep behind, of a
 * tracked off-screen buffer or an effect layer still shows it: those are kept for a later frame.
 */
void Collect()
{
	if (!UnloadArt) return;
	const std::lock_guard<std::mutex> lock(ImagesMutex);
	const uint32_t now = UseClock.load(std::memory_order_relaxed);
	const uint32_t view = ViewClock.load(std::memory_order_relaxed);
	const auto due = [now, view](const HdImage &img) { return view - img.letGoView >= GraceViews || now - img.letGoPresent >= GracePresents; };
	const bool over = BudgetBytes != 0 && ResidentBytes > BudgetBytes && now - LastBudgetScan >= 30;
	if (!over && std::none_of(PendingRelease.begin(), PendingRelease.end(), [&due](uint32_t id) { return Images[id].refs == 0 && due(Images[id]); })) return;
	std::vector<uint8_t> shown(Images.size(), 0);
	const auto mark = [&shown](const std::vector<PixelProvenance> &plane) {
		for (const PixelProvenance &p : plane)
			if (p.image != 0 && p.image < shown.size()) shown[p.image] = 1;
	};
	mark(Plane);
	for (size_t i = 0; i < BehindProv.size() && i < Plane.size(); ++i) { // what is behind a pixel shows only behind a free or blended one
		const PixelProvenance &p = BehindProv[i];
		if (p.image != 0 && p.image < shown.size() && (Plane[i].free != 0 || Plane[i].blend != 0)) shown[p.image] = 1;
	}
	for (const Offscreen &o : Offscreens)
		mark(o.plane);
	for (const Overlay &o : Overlays)
		shown[o.fx] = 1;
	std::vector<uint32_t> keep;
	for (size_t i = 0; i < PendingRelease.size(); ++i) { // (letting go of an image lets go of its effect layer: added at the end)
		const uint32_t id = PendingRelease[i];
		HdImage &img = Images[id];
		if (img.refs != 0) { // held again
			img.pending = false;
			continue;
		}
		if (shown[id] != 0 || !due(img)) {
			keep.push_back(id);
			continue;
		}
		img.pending = false;
		Unload(img);
		if (img.fx != 0) LetGo(img.fx);
		img.fx = 0;
		img.fxLooked = false;
	}
	PendingRelease = std::move(keep);
	if (!over) return;
	LastBudgetScan = now;
	std::vector<uint32_t> idle;
	for (uint32_t id = 1; id < Images.size(); ++id) {
		const HdImage &img = Images[id];
		if (img.resident && shown[id] == 0 && now - img.lastUse >= IdlePresents) idle.push_back(id);
	}
	std::sort(idle.begin(), idle.end(), [](uint32_t a, uint32_t b) { return Images[a].lastUse < Images[b].lastUse; });
	for (const uint32_t id : idle) {
		if (ResidentBytes <= BudgetBytes) break;
		Unload(Images[id]); // still held: drawn again, it is loaded again
	}
}

void Compose(SDL_Surface *pal)
{
	Frame.resize(static_cast<size_t>(pal->w) * Scale * pal->h * Scale);
	const int threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1U, 8U));
	const int rows = (pal->h + threads - 1) / threads;
	const bool havePlane = PlanePitch == pal->pitch && PlaneHeight == pal->h;
	const bool grading = havePlane && !PixelShading;
	const uint16_t cycled = CycledRange.load(std::memory_order_relaxed);
	for (int i = 0; i < 256; ++i)
		Cycled[i] = i >= (cycled & 0xFF) && i <= (cycled >> 8);
	DrawGrades.clear();
	LevelGraded.clear();
	std::vector<BandSums> sums(grading ? threads : 0);
	for (BandSums &band : sums)
		band.levels.resize(static_cast<size_t>(PaletteCount.load()) * 16);
	// Each thread prepares its band of rows (PrepareRows), then one merges the draws' sums, then each composes its band: no thread composes
	// before every band is prepared, as a local grade reads the rows next to its own
	std::barrier prepared(threads, [&sums, pal]() noexcept {
		if (!sums.empty()) PrepareGrades(sums, pal->format->palette->colors);
	});
	const auto work = [&](int t) {
		const int y0 = t * rows;
		const int y1 = std::min(pal->h, y0 + rows);
		if (havePlane && y0 < y1) PrepareRows(pal, y0, y1, grading ? &sums[t] : nullptr);
		prepared.arrive_and_wait();
		if (y0 < y1) ComposeRows(pal, y0, y1);
	};
	std::vector<std::thread> workers;
	for (int t = 1; t < threads; ++t)
		workers.emplace_back(work, t);
	work(0);
	for (std::thread &w : workers)
		w.join();
	if (!Overlays.empty()) ComposeOverlays(pal);
}

void WriteDump(SDL_Surface *pal, const std::string &name)
{
	SDL_Surface *hdSurface = SDL_CreateRGBSurfaceWithFormatFrom(Frame.data(), pal->w * Scale, pal->h * Scale, 32,
	    pal->w * Scale * 4, SDL_PIXELFORMAT_ARGB8888);
	if (hdSurface != nullptr) {
		IMG_SavePNG(hdSurface, (DumpDir + "/hd-" + name + ".png").c_str());
		SDL_FreeSurface(hdSurface);
	}
	IMG_SavePNG(pal, (DumpDir + "/low-" + name + ".png").c_str());
	if (DumpIds && PlanePitch == pal->pitch && PlaneHeight == pal->h) {
		std::vector<uint32_t> ids(static_cast<size_t>(pal->w) * pal->h);
		const auto *pixels = static_cast<const uint8_t *>(pal->pixels);
		for (int y = 0; y < pal->h; ++y)
			for (int x = 0; x < pal->w; ++x) {
				const PixelProvenance &p = Plane[static_cast<size_t>(y) * pal->pitch + x];
				// A = the art's source index, RGB = the image id (0 when the pixel no longer holds what it wrote)
				ids[static_cast<size_t>(y) * pal->w + x] = (static_cast<uint32_t>(p.source) << 24) | (p.written == pixels[static_cast<size_t>(y) * pal->pitch + x] && !Stale(p, x, y) ? p.image : 0);
			}
		SDL_Surface *idSurface = SDL_CreateRGBSurfaceWithFormatFrom(ids.data(), pal->w, pal->h, 32, pal->w * 4, SDL_PIXELFORMAT_ARGB8888);
		if (idSurface != nullptr) {
			IMG_SavePNG(idSurface, (DumpDir + "/ids-" + name + ".png").c_str());
			SDL_FreeSurface(idSurface);
		}
	}
}

void Dump(SDL_Surface *pal, int dumpStep)
{
	if (DumpDir.empty()) return;
	if (!RequestedDump.empty()) {
		WriteDump(pal, RequestedDump);
		RequestedDump.clear();
	}
	if (DumpEvery <= 0) return;
	const int step = dumpStep >= 0 ? dumpStep : PresentCount;
	if (step % DumpEvery != 0 || step == LastDumpStep) return;
	LastDumpStep = step;
	char name[32];
	std::snprintf(name, sizeof(name), "%05d", step / DumpEvery);
	WriteDump(pal, name);
}

} // namespace

bool Enabled()
{
	return Active;
}

namespace {
/** "zombie\\zombie" -> "monsters/zombie/zombie": a monster's sprite path as the pack's "light" lines name it. */
std::string LightBody(std::string_view spritePath)
{
	std::string body = "monsters/";
	for (const char c : spritePath)
		body += c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return body;
}
} // namespace

int MonsterLight(std::string_view spritePath, std::string_view trnFile)
{
	if (!Active || MonsterLights.empty() || spritePath.empty()) return 0;
	const std::string body = LightBody(spritePath);
	if (!trnFile.empty()) {
		const auto own = MonsterLights.find(body + "@" + TrnStem(trnFile));
		if (own != MonsterLights.end()) return own->second;
	}
	const auto it = MonsterLights.find(body);
	return it != MonsterLights.end() ? it->second : 0;
}

int UniqueMonsterLight(std::string_view spritePath, std::string_view trnFile, std::string_view uniqueTrn, std::string_view uniqueName)
{
	if (!Active || MonsterLights.empty() || spritePath.empty() || uniqueTrn.empty()) return 0;
	// The keys of his own art (SpriteScope): the type's swap ("" for none), "+" his TRN, and his slug when he has one
	const std::string shared = LightBody(spritePath) + "@" + (trnFile.empty() ? std::string() : TrnStem(trnFile)) + "+" + TrnStem(uniqueTrn);
	if (const std::string slug = NameSlug(uniqueName); !slug.empty()) {
		const auto own = MonsterLights.find(shared + "~" + slug);
		if (own != MonsterLights.end()) return own->second;
	}
	const auto it = MonsterLights.find(shared);
	return it != MonsterLights.end() ? it->second : 0;
}

namespace {
std::string ConfiguredPack; ///< SetPackPath
} // namespace

void SetPackPath(std::string_view path, std::string_view base)
{
	ConfiguredPack.clear();
	if (path.empty()) return;
	if (IsAbsolutePath(path) || base.empty()) {
		ConfiguredPack = std::string(path);
		return;
	}
	ConfiguredPack = std::string(base);
	if (ConfiguredPack.back() != '/' && ConfiguredPack.back() != '\\') ConfiguredPack += '/';
	ConfiguredPack += path;
}

std::string PackPath()
{
	const char *env = std::getenv("DEVILUTIONX_HD_PACK");
	if (env != nullptr && *env != '\0') return env;
	return ConfiguredPack;
}

int PackScale()
{
	const std::string dir = PackPath();
	if (!Supported || dir.empty()) return 1;
	OpenPack(dir);
	std::vector<uint8_t> text;
	if (!ReadPackFile("hd.txt", text)) return 1;
	std::istringstream index(std::string(text.begin(), text.end()));
	std::string line;
	while (std::getline(index, line)) {
		std::istringstream words(line);
		std::string kind;
		int scale = 0;
		if (words >> kind >> scale && kind == "scale") return std::clamp(scale, 1, 8);
	}
	return 1;
}

namespace {
/** The rule "a" (anything else: off), with its strength. */
void SetMinBright(std::string_view mode, double strength)
{
	MinBright = mode == "a" ? static_cast<float>(std::clamp(strength, 0.0, 1.0)) : 0;
}

/** Just enough JSON for a held folder's render.json: "sprites" at the top, each {"dirs": [...], "frames": n}; everything else skipped. */
class HeldJson {
public:
	explicit HeldJson(const std::string &text)
	    : s(text)
	{
	}

	bool Read(std::unordered_map<std::string, HeldKit> &kits)
	{
		bool ok = Object([&](const std::string &key) {
			if (key != "sprites") return Skip();
			return Object([&](const std::string &path) {
				HeldKit kit;
				const bool read = Object([&](const std::string &field) {
					if (field == "frames") {
						const long n = Number();
						if (n < 0) return false;
						kit.frames = static_cast<uint16_t>(std::min(n, 4096L));
						return true;
					}
					if (field != "dirs") return Skip();
					return Array([&]() {
						const long d = Number();
						if (d < 0) return false;
						if (d < 8) kit.dirs |= static_cast<uint8_t>(1U << d);
						return true;
					});
				});
				if (!read) return false;
				// a path the game loads hero art from, nothing that leaves the folder
				if (path.compare(0, 7, "plrgfx/") == 0 && path.find("..") == std::string::npos && path.find('\\') == std::string::npos && kit.dirs != 0 && kit.frames != 0)
					kits[path] = kit;
				return true;
			});
		});
		return ok;
	}

private:
	const std::string &s;
	size_t i = 0;

	void Space()
	{
		while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0)
			++i;
	}

	bool Is(char c)
	{
		Space();
		if (i < s.size() && s[i] == c) {
			++i;
			return true;
		}
		return false;
	}

	bool String(std::string *out)
	{
		if (!Is('"')) return false;
		while (i < s.size() && s[i] != '"') {
			if (s[i] == '\\') ++i; // an escape: the names that matter here have none
			if (i < s.size() && out != nullptr) out->push_back(s[i]);
			++i;
		}
		return i++ < s.size();
	}

	long Number()
	{
		Space();
		const char *begin = s.c_str() + i;
		char *end = nullptr;
		const long n = std::strtol(begin, &end, 10);
		if (end == begin) return -1;
		i += end - begin;
		while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) != 0 || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-'))
			++i;
		return n;
	}

	template <typename Field>
	bool Object(const Field &field)
	{
		if (!Is('{')) return false;
		if (Is('}')) return true;
		do {
			std::string key;
			if (!String(&key) || !Is(':') || !field(key)) return false;
		} while (Is(','));
		return Is('}');
	}

	template <typename Element>
	bool Array(const Element &element)
	{
		if (!Is('[')) return false;
		if (Is(']')) return true;
		do {
			if (!element()) return false;
		} while (Is(','));
		return Is(']');
	}

	bool Skip()
	{
		Space();
		if (i >= s.size()) return false;
		if (s[i] == '{') return Object([this](const std::string &) { return Skip(); });
		if (s[i] == '[') return Array([this]() { return Skip(); });
		if (s[i] == '"') return String(nullptr);
		const size_t start = i;
		while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && std::isspace(static_cast<unsigned char>(s[i])) == 0)
			++i;
		return i > start;
	}
};

/**
 * Reads the folders under ``folder``'s dir (see "held <dir>" and "pairs <dir>" in hd.hpp): each one's render.json, which says what frames it
 * has. ``line`` is its hd.txt line, ``override`` its environment variable (set: it wins, empty: none).
 */
void LoadHeroArt(HeroArtFolder &folder, std::string line, const char *override)
{
	folder.dir.clear();
	folder.rel.clear();
	folder.kits.clear();
	folder.kitCount = 0;
	if (const char *dir = std::getenv(override); dir != nullptr) line = dir;
	if (line.empty()) return;
	const bool absolute = IsAbsolutePath(line);
	folder.dir = absolute ? line : PackDir + "/" + line;
	while (folder.dir.size() > 1 && (folder.dir.back() == '/' || folder.dir.back() == '\\'))
		folder.dir.pop_back();
	if (!absolute) {
		folder.rel = line;
		while (folder.rel.compare(0, 2, "./") == 0)
			folder.rel.erase(0, 2);
		while (!folder.rel.empty() && (folder.rel.back() == '/' || folder.rel.back() == '\\'))
			folder.rel.pop_back();
		if (folder.rel.empty() || folder.rel == ".") folder.dir.clear(); // the pack itself isn't a folder of hero art
		if (folder.dir.empty()) return;
	}
	if (HeldPalette.empty()) {
		for (const auto &[file, palette] : Manifest) {
			if (file.compare(0, 14, "sprite/plrgfx/") != 0) continue;
			HeldPalette = palette;
			break;
		}
	}
	uint32_t items = 0;
	std::vector<std::string> keys;
	if (LooseFiles || absolute) keys = ListDirectories(folder.dir.c_str());
	if (!absolute) {
		for (const auto &archive : Archives)
			archive->ListDirectories(folder.rel, keys);
	}
	std::sort(keys.begin(), keys.end());
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	for (const std::string &key : keys) {
		if (key.empty() || key[0] == '.' || key.find_first_of("/\\") != std::string::npos) continue;
		std::vector<uint8_t> json;
		if (!ReadPackFile(folder.prefix + key + "/render.json", json)) continue; // unfinished (the renderer writes render.json last)
		const std::string text(json.begin(), json.end());
		std::unordered_map<std::string, HeldKit> kits;
		if (!HeldJson(text).Read(kits)) {
			LogError("HD: can't read {}/{}/render.json", folder.dir, key);
			continue;
		}
		if (kits.empty()) continue;
		for (const auto &[path, kit] : kits)
			ArtStems.insert("sprite/" + path); // the game's sprites of that file are registered (RegisterSpriteList) even without art of the set's
		folder.kitCount += static_cast<uint32_t>(kits.size());
		folder.kits[key] = std::move(kits);
		++items;
	}
	LogInfo("HD: {} art of {} keys ({} sprite files) in {}", folder.prefix.substr(0, folder.prefix.size() - 1), items, folder.kitCount, folder.dir);
}
} // namespace

void Init()
{
	const std::string dir = PackPath();
	if (!Supported || dir.empty()) return;
	const auto initStart = std::chrono::steady_clock::now();
	OpenPack(dir);
	std::vector<uint8_t> text;
	if (!ReadPackFile("hd.txt", text)) {
		LogError("HD: no hd.txt in {}", PackDir);
		return;
	}
	std::istringstream index(std::string(text.begin(), text.end()));
	std::string line;
	std::string held;
	std::string pairs;
	FreeSilhouette = false;
	PixelShading = false;
	MonsterLights.clear();
	MinBright = 0;
	TilePeriods.clear();
	TileHashes.clear();
	PropFloorLines.clear();
	{
		const std::lock_guard<std::mutex> lock(ImagesMutex);
		PropsTileset.clear(); // read again at the next SetTileset
		Props.clear();
		PropFlags.clear();
		PropOwn.clear();
		PropFeetNow.clear();
		PropsNow = false;
	}
	BudgetBytes = DefaultBudgetMb << 20;
	while (std::getline(index, line)) {
		std::istringstream words(line);
		std::string kind;
		words >> kind;
		if (kind == "scale") {
			words >> Scale;
		} else if (kind == "silhouette") {
			std::string mode;
			words >> mode;
			FreeSilhouette = mode == "free";
		} else if (kind == "shading") {
			std::string mode;
			words >> mode;
			PixelShading = mode == "pixel";
		} else if (kind == "minbright") {
			std::string mode;
			double strength = 0;
			words >> mode >> strength;
			SetMinBright(mode, strength);
		} else if (kind == "image") {
			std::string file, palette;
			words >> file >> palette;
			Manifest[file] = palette == "-" ? "" : palette;
		} else if (kind == "light") {
			std::string who;
			int radius = 0;
			if (words >> who >> radius && radius > 0) MonsterLights[who] = std::min(radius, 15);
		} else if (kind == "memory") {
			double mb = 0;
			if (words >> mb) BudgetBytes = static_cast<size_t>(std::max(0.0, mb) * (1 << 20));
		} else if (kind == "tileperiod" || kind == "tilehash") {
			std::string tileset;
			int n = 0;
			if (words >> tileset >> n && n > 0) (kind == "tileperiod" ? TilePeriods : TileHashes)[tileset] = n;
		} else if (kind == "propfloor") {
			std::string tileset, range;
			words >> tileset;
			while (words >> range) {
				int a = -1, b = -1;
				const size_t dash = range.find('-');
				a = std::atoi(range.c_str());
				b = dash == std::string::npos ? a : std::atoi(range.c_str() + dash + 1);
				if (a >= 0 && b >= a && b < 65536) PropFloorLines[tileset].emplace_back(a, b);
			}
		} else if (kind == "held") {
			words >> held;
		} else if (kind == "pairs") {
			words >> pairs;
		}
	}
	Scale = std::clamp(Scale, 1, 8);
	Images.reserve(Manifest.size() + 1 + HeldImageCount);
	ItemUniqueNames.clear();
	ItemUniqueArts.clear();
	++ItemUniqueGeneration;
	for (const auto &[file, palette] : Manifest) {
		if (file.rfind("sprite/", 0) != 0) continue;
		std::string stem = file.substr(0, file.rfind('/'));
		const size_t swap = stem.find('@', stem.rfind('/'));
		if (swap != std::string::npos) {
			if (stem.size() > swap + 6 && stem[swap + 1] == 'c' && stem[swap + 5] == '~') ItemUniqueNames.insert(stem.substr(swap + 1)); // "@c057~<slug>"
			ArtStems.insert(stem); // files registered under an "@" name of their own: Hellfire's menu files ("ui_art/heros@hf", load_pcx.cpp)
			stem.resize(swap);
		}
		ArtStems.insert(std::move(stem));
	}
	{
		const std::lock_guard<std::mutex> lock(ImagesMutex);
		HeldFolder.dir.clear();
		HeldFolder.rel.clear();
		HeldFolder.kits.clear();
		HeldFolder.kitCount = 0;
		PairFolder.dir.clear();
		PairFolder.rel.clear();
		PairFolder.kits.clear();
		PairFolder.kitCount = 0;
		HeldBy.clear();
		++HeldGeneration;
	}
	HeldPalette.clear();
	LoadHeroArt(HeldFolder, held, "DEVILUTIONX_HD_HELD");
	LoadHeroArt(PairFolder, pairs, "DEVILUTIONX_HD_PAIRS");
	if (const char *dump = std::getenv("DEVILUTIONX_HD_DUMP"); dump != nullptr) {
		DumpDir = dump;
		const char *every = std::getenv("DEVILUTIONX_HD_DUMP_EVERY");
		DumpEvery = every != nullptr ? std::max(0, std::atoi(every)) : 60;
		DumpIds = std::getenv("DEVILUTIONX_HD_DUMP_IDS") != nullptr;
	}
	if (const char *floor = std::getenv("DEVILUTIONX_HD_MINBRIGHT"); floor != nullptr && *floor != '\0') {
		const std::string text = floor;
		const size_t colon = text.find(':');
		SetMinBright(text.substr(0, colon), colon == std::string::npos ? 0 : std::atof(text.c_str() + colon + 1));
	}
	if (MinBright > 0)
		LogInfo("HD: minimum brightness for monsters, {}", MinBright);
	if (const char *unload = std::getenv("DEVILUTIONX_HD_UNLOAD"); unload != nullptr && *unload != '\0') UnloadArt = std::atoi(unload) != 0;
	if (const char *indexed = std::getenv("DEVILUTIONX_HD_INDEXED"); indexed != nullptr && *indexed != '\0') IndexedArt = std::atoi(indexed) != 0;
	if (const char *budget = std::getenv("DEVILUTIONX_HD_MEMORY_MB"); budget != nullptr && *budget != '\0') BudgetBytes = static_cast<size_t>(std::max(0.0, std::atof(budget)) * (1 << 20));
	if (const char *log = std::getenv("DEVILUTIONX_HD_MEMLOG"); log != nullptr) MemLog = log;
	Active = true;
	{
		// a level may have set its tileset before the pack loaded (or another pack): its props are this pack's
		const std::lock_guard<std::mutex> lock(ImagesMutex);
		PropsTileset = Tileset + "/" + std::to_string(TilesetRows);
		ReadProps(Tileset, TilesetRows);
	}
	SurfaceBlitHook = &CopyProvenance;
	if (ClxFreeHook == nullptr) {
		ClxFreeHook = [](const uint8_t *data) { ForgetSprites(data); };
		std::atexit([]() { ClxFreeHook = nullptr; }); // sprite data freed at exit may outlive the maps here
	}
	LogInfo("HD: pack {} ({} images, {} archives{}, {}x{}{})", PackDir, Manifest.size(), Archives.size(), LooseFiles ? " under its loose files" : "", Scale, FreeSilhouette ? ", free silhouettes" : "", PixelShading ? ", shaded per pixel" : "");
	LogInfo("HD: pack read in {:.0f} ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - initStart).count());
	LogInfo("HD: images {}, idle art bounded at {} MB, {}", UnloadArt ? "let go of when unused" : "kept once loaded", BudgetBytes >> 20,
	    IndexedArt ? "8-bit where 256 colours do" : "all in full colour");
}

void RegisterSprites(std::string_view path, ClxSpriteListOrSheet sprites, const void *owner)
{
	if (!Active) return;
	if (sprites.isSheet()) {
		const ClxSpriteSheet sheet = sprites.sheet();
		for (uint16_t g = 0; g < sheet.numLists(); ++g)
			RegisterSpriteList(path, g, sheet[g], owner != nullptr ? owner : sheet.data());
	} else {
		RegisterSpriteList(path, 0, sprites.list(), owner);
	}
}

void RegisterSpriteList(std::string_view path, uint16_t group, ClxSpriteList list, const void *owner)
{
	if (!Active) return;
	if (owner == nullptr) owner = list.data();
	const std::string stem = "sprite/" + NormalizePath(path);
	const bool art = ArtStems.count(stem) != 0;
	const std::lock_guard<std::mutex> lock(ImagesMutex);
	for (uint32_t f = 0; f < list.numSprites(); ++f) {
		const ClxSprite sprite = list[f];
		const auto found = Sprites.find(sprite.pixelData());
		if (found != Sprites.end()) { // a buffer reused at the same address (one freed without telling, as before ClxFreeHook)
			LetGoOf(found->second);
			if (!art) Sprites.erase(found);
		}
		if (!art) continue;
		SpriteEntry &entry = Sprites[sprite.pixelData()];
		entry = SpriteEntry {};
		entry.stem = stem;
		entry.frame = "/" + std::to_string(group) + "." + std::to_string(f) + ".png";
		entry.check = Checksum(sprite);
		entry.owner = owner;
		Owners[owner].push_back(sprite.pixelData());
	}
}

void ForgetSprites(const void *owner)
{
	if (!Active || owner == nullptr) return;
	const std::lock_guard<std::mutex> lock(ImagesMutex);
	const auto found = Owners.find(owner);
	if (found == Owners.end()) return;
	for (const uint8_t *key : found->second) {
		const auto entry = Sprites.find(key);
		if (entry == Sprites.end() || entry->second.owner != owner) continue; // registered again since, for data of its own
		LetGoOf(entry->second);
		Sprites.erase(entry);
	}
	Owners.erase(found);
}

MemoryUse Memory()
{
	const std::lock_guard<std::mutex> lock(ImagesMutex);
	MemoryUse use;
	use.bytes = ResidentBytes;
	use.peak = PeakBytes;
	use.loads = LoadCount;
	use.unloads = UnloadCount;
	for (const HdImage &img : Images) {
		use.images += img.resident ? 1 : 0;
		use.indexed += img.resident && !img.idx.empty() ? 1 : 0;
	}
	return use;
}

MemoryUse Memory(std::string_view file)
{
	const std::lock_guard<std::mutex> lock(ImagesMutex);
	MemoryUse use;
	const auto found = ImageIds.find(std::string(file));
	if (found == ImageIds.end() || found->second == 0 || !Images[found->second].resident) return use;
	const HdImage &img = Images[found->second];
	use.bytes = img.bytes;
	use.images = 1;
	use.indexed = img.idx.empty() ? 0 : 1;
	return use;
}

bool Loaded(std::string_view file)
{
	const std::lock_guard<std::mutex> lock(ImagesMutex);
	const auto found = ImageIds.find(std::string(file));
	return found != ImageIds.end() && found->second != 0 && Images[found->second].resident;
}

void BeforeRecolour(ClxSprite sprite, const uint8_t *trn)
{
	if (!Active) return;
	const std::lock_guard<std::mutex> lock(ImagesMutex);
	const auto found = Sprites.find(sprite.pixelData());
	if (found == Sprites.end() || found->second.check != Checksum(sprite)) return;
	SpriteEntry &entry = found->second;
	entry.pending = true;
	if (entry.swapArt) { // recoloured again: its swap's art no longer shows it
		entry.swapArt = false;
		entry.recolour.clear();
	} else if (!RecolourName.empty() && entry.recolour.empty() && entry.original.empty()) {
		entry.recolour = RecolourName;
		entry.swapArt = Manifest.count(entry.Key(RecolourName)) != 0;
		if (entry.swapArt) return; // drawn from the recoloured pixels as they are
	}
	if (Manifest.count(entry.Key({})) == 0) return; // no art of its own to grade through the recolour
	if (entry.original.empty()) {
		entry.original.assign(sprite.pixelData(), sprite.pixelData() + sprite.pixelDataSize());
		std::copy_n(trn, 256, entry.trn.begin());
	} else {
		for (uint8_t &c : entry.trn) // recoloured again: compose
			c = trn[c];
	}
}

void AfterRecolour(ClxSprite sprite)
{
	if (!Active) return;
	const std::lock_guard<std::mutex> lock(ImagesMutex);
	const auto found = Sprites.find(sprite.pixelData());
	if (found != Sprites.end() && found->second.pending) {
		found->second.check = Checksum(sprite);
		found->second.pending = false;
	}
}

NamedRecolour::NamedRecolour(std::string_view name)
{
	RecolourName = TrnStem(name);
}

NamedRecolour::~NamedRecolour()
{
	RecolourName.clear();
}

void NameTrn(const uint8_t *trn, std::string_view name, std::string_view owner)
{
	if (!Active || trn == nullptr) return;
	std::string stem = TrnStem(name);
	const std::string slug = NameSlug(owner);
	std::string own = slug.empty() ? std::string() : stem + "~" + slug;
	const std::lock_guard<std::mutex> lock(ImagesMutex);
	TrnNames[trn] = { std::move(stem), std::move(own), TrnChecksum(trn) };
}

ItemGraphic::ItemGraphic(int cursorGraphic)
    : ItemGraphic(cursorGraphic, true, -1, {})
{
}

ItemGraphic::ItemGraphic(int cursorGraphic, bool dropped, int uniqueIndex, std::string_view uniqueName)
{
	if (!Active || cursorGraphic < 0) return;
	char name[16];
	std::snprintf(name, sizeof(name), "c%03d", cursorGraphic);
	if (dropped) ItemArt = name;
	if (ItemUniqueNames.empty() || uniqueIndex < 0 || uniqueIndex >= 4096 || uniqueName.empty()) return;
	if (ItemUniqueArts.size() <= static_cast<size_t>(uniqueIndex)) ItemUniqueArts.resize(uniqueIndex + 1);
	UniqueItemArt &art = ItemUniqueArts[uniqueIndex];
	if (art.cursor != cursorGraphic || art.source != uniqueName) {
		art.cursor = cursorGraphic;
		art.source = uniqueName;
		art.name = std::string(name) + "~" + NameSlug(uniqueName);
		if (ItemUniqueNames.count(art.name) == 0) art.name.clear();
		++ItemUniqueGeneration;
	}
	if (!art.name.empty()) ItemUnique = &art;
}

ItemGraphic::~ItemGraphic()
{
	ItemArt.clear();
	ItemUnique = nullptr;
}

HeldItem::HeldItem(const void *who, int cursorGraphic, int uniqueIndex, std::string_view uniqueName)
    : HeldItem(who, HeldPiece { cursorGraphic, uniqueIndex, uniqueName }, HeldPiece {})
{
}

HeldItem::HeldItem(const void *who, HeldPiece weapon, HeldPiece shield)
{
	if (!Active || who == nullptr || (HeldFolder.kits.empty() && PairFolder.kits.empty())) return;
	// The names an item's art may be under, most specific first: its unique's own ("c057~doombringer"), its picture's ("c057")
	const auto names = [](const HeldPiece &piece) {
		std::vector<std::string> out;
		if (piece.cursorGraphic < 0) return out;
		char name[16];
		std::snprintf(name, sizeof(name), "c%03d", piece.cursorGraphic);
		if (piece.uniqueIndex >= 0 && !piece.uniqueName.empty()) out.push_back(std::string(name) + "~" + NameSlug(piece.uniqueName));
		out.emplace_back(name);
		return out;
	};
	const HeroArt level = HeroArtLevel.load();
	const std::vector<std::string> weapons = names(weapon);
	std::vector<std::string> folders; // where to look, most specific first; none: the set's art, as before
	if (level >= HeroArt::UniqueWeaponsAndShields && shield.cursorGraphic >= 0 && !PairFolder.kits.empty()) {
		const std::vector<std::string> shields = names(shield);
		for (const std::string &w : weapons.empty() ? std::vector<std::string> { std::string() } : weapons) {
			for (const std::string &s : shields) {
				const std::string key = w.empty() ? s : w + "+" + s; // "c057+c083", or a shield alone's "c083"
				if (PairFolder.kits.count(key) != 0) folders.push_back(PairFolder.prefix + key);
			}
		}
	}
	// The weapon's own art: with a shield only where pairs are looked for (a weapon set with a shield is a set of its own; its art falls
	// back on the weapon's only where the held art has frames of it)
	if (level >= HeroArt::UniqueWeapons && (shield.cursorGraphic < 0 || level >= HeroArt::UniqueWeaponsAndShields)) {
		for (const std::string &w : weapons)
			if (HeldFolder.kits.count(w) != 0) folders.push_back(HeldFolder.prefix + w);
	}
	const std::lock_guard<std::mutex> lock(ImagesMutex);
	const HeldChoice *choice = nullptr;
	if (!folders.empty()) {
		std::string key;
		for (const std::string &folder : folders)
			key += (key.empty() ? "" : "|") + folder;
		auto found = HeldChoices.find(key);
		if (found == HeldChoices.end()) found = HeldChoices.emplace(key, HeldChoice { std::move(folders) }).first;
		choice = &found->second;
	}
	HeroHeld &hero = HeldBy[who];
	if (hero.choice != choice) LetGoOfHeld(who, hero); // another item (or none): what the frames drew of the last one is let go of
	hero.choice = choice;
	if (choice == nullptr) return;
	HeldNow = choice;
	HeldHero = &hero;
	HeldWho = who;
}

HeldItem::~HeldItem()
{
	HeldNow = nullptr;
	HeldHero = nullptr;
	HeldWho = nullptr;
}

void LetGoOfHeldArt(const void *who)
{
	if (!Active) return;
	const std::lock_guard<std::mutex> lock(ImagesMutex);
	const auto found = HeldBy.find(who);
	if (found != HeldBy.end()) LetGoOfHeld(who, found->second);
}

std::string HeldArt(const void *who)
{
	const std::lock_guard<std::mutex> lock(ImagesMutex);
	const auto found = HeldBy.find(who);
	if (found == HeldBy.end() || found->second.choice == nullptr) return {};
	const std::string &folder = found->second.choice->folders.front();
	return folder.substr(folder.find('/') + 1);
}

HeldUse Held()
{
	const std::lock_guard<std::mutex> lock(ImagesMutex);
	HeldUse use;
	use.items = static_cast<uint32_t>(HeldFolder.kits.size());
	use.kits = HeldFolder.kitCount;
	use.pairs = static_cast<uint32_t>(PairFolder.kits.size());
	use.pairKits = PairFolder.kitCount;
	for (const auto &[key, entry] : Sprites)
		use.frames += entry.heldImage != 0 && entry.heldGeneration == HeldGeneration ? 1 : 0; // (of this pack)
	return use;
}

void SetHeroArt(HeroArt level)
{
	const std::lock_guard<std::mutex> lock(ImagesMutex);
	if (HeroArtLevel.load() == level) return;
	HeroArtLevel.store(level);
	// What heroes drew at the old level is let go of (loaded again when drawn again), and what they hold is looked up again (HeldItem)
	for (auto &[who, hero] : HeldBy)
		LetGoOfHeld(who, hero);
	++HeldGeneration;
	if (level != HeroArt::Original) return;
	for (auto &[key, entry] : Sprites) {
		if (entry.stem.compare(0, 14, "sprite/plrgfx/") != 0) continue;
		for (const uint32_t id : entry.images)
			LetGo(id);
		entry.images.clear();
	}
}

HeroArt GetHeroArt()
{
	return HeroArtLevel.load();
}

void SetTarget(SDL_Surface *palSurface)
{
	Target = palSurface;
}

void TrackOffscreen(SDL_Surface *buffer)
{
	if (!Active || buffer == nullptr) return;
	// The level loader thread tracks the panel's buffers (InitMainPanel, on a game's first level) while the main thread presents the loading
	// screen, and Collect (under ImagesMutex) reads every tracked plane: adding to Offscreens moved them under it.
	const std::lock_guard<std::mutex> lock(ImagesMutex);
	Offscreen *off = FindOffscreen(buffer);
	if (off == nullptr) off = &Offscreens.emplace_back(Offscreen { buffer, {} });
	off->plane.assign(static_cast<size_t>(buffer->pitch) * buffer->h, PixelProvenance {});
}

void ForgetOffscreen(SDL_Surface *buffer)
{
	const std::lock_guard<std::mutex> lock(ImagesMutex); // (see TrackOffscreen)
	Offscreens.erase(std::remove_if(Offscreens.begin(), Offscreens.end(), [buffer](const Offscreen &o) { return o.surface == buffer; }), Offscreens.end());
}

namespace {

/** Reads the props of the tileset ``name`` (pillars of ``rows`` rows) from the pack: PropFlags, PropOwn, PropFeetNow. Under ImagesMutex. */
void ReadProps(std::string_view name, int rows)
{
	Props.clear();
	PropFlags.clear();
	PropOwn.clear();
	PropFeetNow.clear();
	PropsNow = false;
	if (!Active || name.empty()) return;
	const std::string prefix = "prop/" + std::string(name) + "/";
	const auto flag = [](size_t pillar, uint8_t bits) {
		if (pillar >= PropFlags.size()) PropFlags.resize(pillar + 1, 0);
		PropFlags[pillar] |= bits;
	};
	for (const auto &[file, palette] : Manifest) {
		if (file.compare(0, prefix.size(), prefix) != 0) continue;
		const char *stem = file.c_str() + prefix.size();
		char *end = nullptr;
		const unsigned long pillar = std::strtoul(stem, &end, 10);
		if (end == stem || pillar >= 65536 || (*end != '@' && std::strcmp(end, ".png") != 0)) continue;
		const std::string text = PackText(file, "hdprop");
		Prop prop;
		if (!ParseProp(text, rows, prop)) {
			LogError("HD: {} doesn't say where it goes (tEXt hdprop \"{}\")", file, text);
			continue;
		}
		prop.file = file;
		const Prop &kept = Props.emplace(file, std::move(prop)).first->second;
		flag(pillar, PropHas | (kept.keep ? 0 : PropClears) | (*end == '@' ? PropVariants : 0));
		if (*end != '@') {
			if (pillar >= PropOwn.size()) PropOwn.resize(pillar + 1, nullptr);
			PropOwn[pillar] = &kept;
		}
		if (std::find(PropFeetNow.begin(), PropFeetNow.end(), kept.foot) == PropFeetNow.end()) PropFeetNow.push_back(kept.foot);
	}
	if (const auto lines = PropFloorLines.find(std::string(name)); lines != PropFloorLines.end())
		for (const auto &[a, b] : lines->second)
			for (int pillar = a; pillar <= b; ++pillar)
				flag(static_cast<size_t>(pillar), PropClears);
	// the same order every time (the manifest's isn't): props whose pillars are further back first
	std::sort(PropFeetNow.begin(), PropFeetNow.end(), [](Displacement a, Displacement b) {
		return a.deltaX + a.deltaY != b.deltaX + b.deltaY ? a.deltaX + a.deltaY > b.deltaX + b.deltaY : a.deltaX > b.deltaX;
	});
	PropsNow = !PropFlags.empty();
	if (PropsNow) LogInfo("HD: {} props for {} ({} squares they stand on)", Props.size(), name, PropFeetNow.size());
}

} // namespace

void SetTileset(std::string_view name, int rows)
{
	const std::lock_guard<std::mutex> lock(ImagesMutex);
	if (name != Tileset) { // the pillars of the last tileset are no longer drawn
		for (const uint32_t id : TileHeld)
			LetGo(id);
		TileHeld.clear();
	}
	Tileset = name;
	TilesetRows = rows;
	const std::string key = std::string(name) + "/" + std::to_string(rows);
	if (key != PropsTileset) {
		PropsTileset = key;
		ReadProps(name, rows);
	}
}

uint8_t LightLevelOf(const uint8_t *table)
{
	if (table == nullptr || table < LightTablesBegin || table >= LightTablesEnd) return 0;
	return static_cast<uint8_t>(std::min<ptrdiff_t>((table - LightTablesBegin) / 256, 15));
}

void SetCycledColors(uint8_t first, uint8_t last)
{
	CycledRange.store(static_cast<uint16_t>(first | (last << 8)), std::memory_order_relaxed);
}

void SetLightTables(const uint8_t *tables, size_t size)
{
	LightTablesBegin = tables;
	LightTablesEnd = tables != nullptr ? tables + size : nullptr;
}

void KeepBehind(const uint8_t *dst, unsigned n)
{
	if (TrackPlane == nullptr || TrackPlane != Plane.data() || TrackBase == nullptr) return;
	if (BehindProv.size() != Plane.size()) {
		BehindProv.assign(Plane.size(), PixelProvenance {});
		BehindIdx.assign(Plane.size(), 0);
	}
	if (BlendLit.size() != Plane.size()) BlendLit.assign(Plane.size(), 0);
	const size_t at = static_cast<size_t>(dst - TrackBase);
	if (at + n > Plane.size()) return;
	for (unsigned i = 0; i < n; ++i) {
		PixelProvenance p = Plane[at + i];
		uint8_t index = dst[i];
		if (p.blend != 0) { // blended itself: kept as its art drawn opaque (one layer deep)
			if (p.image != 0 && p.written == index) index = p.written = BlendLit[at + i];
			p.blend = 0;
		}
		BehindProv[at + i] = p;
		BehindIdx[at + i] = index;
	}
}

void MarkBlend(const uint8_t *dst, const uint8_t *lit, unsigned n)
{
	if (!TrackBlend || TrackPlane != Plane.data() || BlendLit.size() != Plane.size()) return;
	const size_t at = static_cast<size_t>(dst - TrackBase);
	if (at + n > Plane.size()) return;
	for (unsigned i = 0; i < n; ++i) {
		PixelProvenance &p = Plane[at + i];
		if (p.serial != TrackSerial) continue; // (foliage under a free sprite's extension: not drawn)
		p.blend = 1;
		BlendLit[at + i] = lit[i];
	}
}

void NewScene()
{
	Overlays.clear();
}

void ForgetView(const Surface &out)
{
	if (!Active || out.surface != Target || !EnsurePlane()) return;
	// Not cleared here (a few MB every frame): the pixels drawn before now are told apart by their serial (Stale) and cleared as the frame
	// is composed
	ViewX0 = std::max(out.region.x, 0);
	ViewY0 = std::max(out.region.y, 0);
	ViewX1 = std::min(out.region.x + out.region.w, Target->w);
	ViewY1 = std::min(out.region.y + out.region.h, Target->h);
	SceneSerial = NextSerial;
	DrawLights.clear();
	++ViewClock;
}

SpriteScope::SpriteScope(const Surface &out, Point position, ClxSprite clx, const uint8_t *drawTrn, bool local)
{
	uint32_t image = 0;
	const SpriteEntry *drawn = nullptr;
	if (Active) {
		const std::lock_guard<std::mutex> lock(ImagesMutex);
		const auto found = Sprites.find(clx.pixelData());
		// (heroes at the hero art level Original: the 1997 sprite, as for a sprite without art)
		if (found != Sprites.end() && found->second.check == Checksum(clx)
		    && (HeroArtLevel.load(std::memory_order_relaxed) != HeroArt::Original || found->second.stem.compare(0, 14, "sprite/plrgfx/") != 0)) {
			SpriteEntry &entry = found->second;
			drawn = &entry;
			// The most specific art the pack has: a hero's pair's or held item's (see HeldItem), a unique item's own, a dropped item
			// graphic's own, the one monster's the draw's TRN belongs to, the draw's own swap on top of the sprite's (art that every monster
			// with that TRN shares), the sprite's swap, itself
			bool held = false;
			if (HeldNow != nullptr && entry.stem.compare(0, 14, "sprite/plrgfx/") == 0) {
				if (entry.heldFor != HeldNow || entry.heldWho != HeldWho || entry.heldGeneration != HeldGeneration) { // (looked up once for each frame and item)
					LetGoOfHeld(entry);
					const std::string path = entry.stem.substr(7) + entry.frame; // "plrgfx/warrior/wls/wlsas/0.3.png"
					uint32_t art = 0;
					for (size_t i = 0; i < HeldNow->folders.size() && art == 0; ++i)
						art = ImageId(HeldNow->folders[i] + "/" + path, clx.width(), clx.height());
					entry.heldFor = HeldNow;
					entry.heldWho = HeldWho;
					entry.heldGeneration = HeldGeneration;
					entry.heldImage = art;
					if (art != 0) {
						Hold(art); // until the hero holds another item, or the game frees the frame
						std::vector<const uint8_t *> &frames = HeldHero->frames;
						if (frames.size() >= 1024 && (frames.size() & (frames.size() - 1)) == 0) { // now and then: forget frames the game has freed
							frames.erase(std::remove_if(frames.begin(), frames.end(), [](const uint8_t *key) {
								const auto other = Sprites.find(key);
								return other == Sprites.end() || other->second.heldWho != HeldWho || other->second.heldImage == 0;
							}),
							    frames.end());
						}
						frames.push_back(clx.pixelData());
					}
				}
				image = entry.heldImage;
				if (image != 0 && !Images[image].resident && !Reload(image)) image = 0; // (its file gone since: the set's art)
				held = image != 0;
			}
			if (image == 0 && ItemUnique != nullptr && entry.recolour.empty()) {
				if (entry.uniqueFor != ItemUnique || entry.uniqueGeneration != ItemUniqueGeneration) { // (looked up once for each frame and unique)
					entry.uniqueImage = ImageId(entry.Key(ItemUnique->name), clx.width(), clx.height());
					entry.uniqueFor = ItemUnique;
					entry.uniqueGeneration = ItemUniqueGeneration;
				}
				image = entry.uniqueImage;
			}
			if (image == 0 && !ItemArt.empty() && entry.recolour.empty())
				image = ImageId(entry.Key(ItemArt), clx.width(), clx.height());
			if (const NamedTrn *unique = image == 0 ? TrnNamed(drawTrn) : nullptr; unique != nullptr) {
				if (!unique->own.empty())
					image = ImageId(entry.Key(entry.recolour + "+" + unique->own), clx.width(), clx.height());
				if (image == 0)
					image = ImageId(entry.Key(entry.recolour + "+" + unique->name), clx.width(), clx.height());
				if (image != 0) marks = drawTrn;
			}
			if (image == 0 && entry.swapArt)
				image = ImageId(entry.Key(entry.recolour), clx.width(), clx.height());
			if (image == 0) {
				image = ImageId(entry.Key({}), clx.width(), clx.height());
				if (image != 0 && !entry.original.empty()) {
					original = entry.original.data();
					trn = entry.trn.data();
				}
			}
			if (image != 0 && !Images[image].resident && !Reload(image)) image = 0; // (a unique item's art is kept by the entry, not looked up)
			if (image != 0) Images[image].lastUse = UseClock.load(std::memory_order_relaxed);
			if (!held) HoldFor(entry, image);
		}
	}
	const int left = out.region.x + position.x;
	const int top = out.region.y + position.y - clx.height() + 1;
	// Art with a box of its own covers that rectangle instead of the sprite's: its top-left is what the pixels record
	const bool boxed = image != 0 && Images[image].boxed;
	Begin(out, image, left + (boxed ? Images[image].boxDx : 0), top + (boxed ? Images[image].boxDy : 0), true);
	if (!PixelShading) {
		// Through a TRN that isn't a light table, the art is recoloured (unless its own art shows the TRN already): graded locally, as are
		// draws whose pixels don't share one light
		const bool lightTable = drawTrn != nullptr && drawTrn >= LightTablesBegin && drawTrn < LightTablesEnd;
		const bool recoloured = original != nullptr || (marks == nullptr && drawTrn != nullptr && !lightTable);
		// Recoloured art of the panels and menus (the spell icons through their TRN) is the original upscaled, pixel for pixel: the plain
		// per-pixel grade is exact for it, and costs less than a local one
		const bool world = image != 0 && Images[image].world;
		TrackShade = recoloured && !world ? ShadePixel : local || recoloured ? ShadeLocal : ShadeDraw;
	}
	if (image == 0 || out.surface != Target || TrackPlane != Plane.data() || TrackImage != image) return; // not tracked in the game's own surface
	if (MinBright > 0 && Images[image].monster) {
		// the light the game lit this monster draw with (a light table's level), for the floor's fade; a TRN that isn't a light table
		// (Infravision, Stone Curse, a unique's colours) is a recolour and not dim light, so the floor leaves it alone
		const bool lit = drawTrn != nullptr && drawTrn >= LightTablesBegin && drawTrn < LightTablesEnd;
		DrawLights[TrackSerial] = drawTrn == nullptr ? 255 : lit ? FloorWeightOfLevel(LightLevelOf(drawTrn)) : 0;
	}
	if (boxed) Free.whole = Free.on;
	uint32_t fx = 0;
	{
		const std::lock_guard<std::mutex> lock(ImagesMutex);
		fx = FxOf(image, *drawn);
	}
	if (fx != 0) {
		Fx = { fx, left, top, std::max(out.region.x, 0), std::max(out.region.y, 0), std::min(out.region.x + out.region.w, Target->w), std::min(out.region.y + out.region.h, Target->h) };
		Free.whole = Free.on;
	}
}

SpriteScope::~SpriteScope()
{
	End();
}

namespace {

/** TileVariantFile for art under ``kind`` ("tile", "prop"). */
std::string VariantFile(std::string_view kind, std::string_view tileset, uint16_t pillar, Point tile, int period, int hashes)
{
	if (tile.x < 0 || tile.y < 0) return {};
	const std::string stem = std::string(kind) + "/" + std::string(tileset) + "/" + std::to_string(pillar) + "@";
	if (period > 0) return stem + std::to_string(tile.x % period) + "-" + std::to_string(tile.y % period) + ".png";
	if (hashes > 0) {
		uint32_t h = static_cast<uint32_t>(tile.x) * 73856093U ^ static_cast<uint32_t>(tile.y) * 19349663U;
		h ^= h >> 13;
		h *= 0x5bd1e995U;
		h ^= h >> 15;
		return stem + "h" + std::to_string(h % static_cast<uint32_t>(hashes)) + ".png";
	}
	return {};
}

} // namespace

std::string TileVariantFile(std::string_view tileset, uint16_t pillar, Point tile, int period, int hashes)
{
	return VariantFile("tile", tileset, pillar, tile, period, hashes);
}

PillarScope::PillarScope(const Surface &out, Point bottomLeft, uint16_t pillar, int rows, Point tile)
{
	uint32_t image = 0;
	if (Active && !Tileset.empty()) {
		const std::lock_guard<std::mutex> lock(ImagesMutex);
		if (!TilePeriods.empty() || !TileHashes.empty()) { // art for this position first, when the pack has some for the tileset
			const auto period = TilePeriods.find(Tileset);
			const auto hashes = TileHashes.find(Tileset);
			const std::string variant = TileVariantFile(Tileset, pillar, tile, period == TilePeriods.end() ? 0 : period->second,
			    hashes == TileHashes.end() ? 0 : hashes->second);
			if (!variant.empty() && Manifest.count(variant) != 0) image = ImageId(variant, 64, rows * 32);
		}
		if (image == 0) image = ImageId("tile/" + Tileset + "/" + std::to_string(pillar) + ".png", 64, rows * 32);
		if (image != 0 && TileHeld.insert(image).second) Hold(image);
	}
	Begin(out, image, out.region.x + bottomLeft.x, out.region.y + bottomLeft.y - rows * 32 + 1);
	if (!PixelShading) TrackShade = ShadeLevel; // lit per piece or per pixel: dun_render marks each pixel's light level
}

PillarScope::~PillarScope()
{
	End();
}

bool ParseProp(std::string_view text, int rows, Prop &prop)
{
	constexpr int Most = 4096;
	rows = std::max(rows, 1);
	std::istringstream words { std::string(text) };
	if (!(words >> prop.dx >> prop.dy >> prop.w >> prop.h) || prop.w <= 0 || prop.h <= 0 || prop.w > Most || prop.h > Most || std::abs(prop.dx) > Most
	    || std::abs(prop.dy) > Most)
		return false;
	prop.footX = 32; // the centre of the pillar's own square
	prop.footY = 32 * rows - 16;
	prop.keep = false;
	std::string word;
	while (words >> word) {
		if (word == "keep") {
			prop.keep = true;
		} else if (word == "foot") {
			if (!(words >> prop.footX >> prop.footY) || std::abs(prop.footX) > Most || std::abs(prop.footY) > Most) return false;
		}
	}
	prop.foot = SquareUnder(prop.footX, prop.footY, rows);
	return true;
}

Displacement SquareUnder(int x, int y, int rows)
{
	// from the centre of the pillar's own square: one square on x is (32, 16) on screen, one on y (-32, 16)
	const double sx = (x - 32) / 32.0;
	const double sy = (y - (32 * std::max(rows, 1) - 16)) / 16.0;
	return { static_cast<int>(std::floor((sy + sx) / 2 + 0.5)), static_cast<int>(std::floor((sy - sx) / 2 + 0.5)) };
}

bool HasProps()
{
	return PropsNow;
}

bool PropFloorOnly(uint16_t pillar)
{
	return pillar < PropFlags.size() && (PropFlags[pillar] & PropClears) != 0;
}

const std::vector<Displacement> &PropFeet()
{
	return PropFeetNow;
}

const Prop *PropAt(uint16_t pillar, Point tile, Displacement foot)
{
	if (pillar >= PropFlags.size() || (PropFlags[pillar] & PropHas) == 0) return nullptr;
	const Prop *prop = pillar < PropOwn.size() ? PropOwn[pillar] : nullptr;
	if ((PropFlags[pillar] & PropVariants) != 0) {
		const auto period = TilePeriods.find(Tileset);
		const auto hashes = TileHashes.find(Tileset);
		const std::string variant = VariantFile("prop", Tileset, pillar, tile, period == TilePeriods.end() ? 0 : period->second, hashes == TileHashes.end() ? 0 : hashes->second);
		if (const auto found = Props.find(variant); found != Props.end()) prop = &found->second;
	}
	return prop != nullptr && prop->foot == foot ? prop : nullptr;
}

namespace {

/**
 * A prop's 8-bit picture (HdImage::propSource): per original pixel where its art is opaque somewhere in the pixel's block (alpha 32 or more, as
 * ComputeReach), the colour of its reference palette nearest the art's mean there (by alpha), leaving out 0 and the colours the level animates;
 * 0 elsewhere. Without a reference palette, the PalSurface's.
 */
void PropSource(HdImage &img)
{
	const int n = Scale;
	const uint16_t cycled = CycledRange.load(std::memory_order_relaxed);
	const int cycledFirst = cycled & 0xFF, cycledLast = cycled >> 8;
	std::array<uint8_t, 768> palette {};
	if (img.hasRef) {
		palette = img.ref;
	} else if (Target != nullptr && Target->format->palette != nullptr) {
		for (int i = 0; i < 256 && i < Target->format->palette->ncolors; ++i) {
			const SDL_Color c = Target->format->palette->colors[i];
			palette[i * 3] = c.r;
			palette[i * 3 + 1] = c.g;
			palette[i * 3 + 2] = c.b;
		}
	}
	std::unordered_map<uint32_t, uint8_t> nearest;
	const auto nearestOf = [&](uint32_t rgb) {
		const auto found = nearest.find(rgb);
		if (found != nearest.end()) return found->second;
		const int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
		int best = 1, bestD = 1 << 30;
		for (int i = 1; i < 256; ++i) {
			if (i >= cycledFirst && i <= cycledLast) continue;
			const int dr = palette[i * 3] - r, dg = palette[i * 3 + 1] - g, db = palette[i * 3 + 2] - b;
			const int d = dr * dr * 2 + dg * dg * 4 + db * db * 3;
			if (d < bestD) {
				bestD = d;
				best = i;
			}
		}
		nearest.emplace(rgb, static_cast<uint8_t>(best));
		return static_cast<uint8_t>(best);
	};
	img.propSource.assign(static_cast<size_t>(img.loW) * img.loH, 0);
	for (int ly = 0; ly < img.loH; ++ly) {
		for (int lx = 0; lx < img.loW; ++lx) {
			uint32_t most = 0, sumA = 0, sumR = 0, sumG = 0, sumB = 0;
			for (int j = 0; j < n; ++j) {
				const int v = std::min(img.h - 1, (ly * n + j) * img.h / (img.loH * n));
				for (int i = 0; i < n; ++i) {
					const int u = std::min(img.w - 1, (lx * n + i) * img.w / (img.loW * n));
					const uint32_t c = Pixel(img, static_cast<size_t>(v) * img.w + u);
					const uint32_t a = c >> 24;
					most = std::max(most, a);
					sumA += a;
					sumR += ((c >> 16) & 0xFF) * a;
					sumG += ((c >> 8) & 0xFF) * a;
					sumB += (c & 0xFF) * a;
				}
			}
			if (most < 32) continue;
			const uint32_t rgb = ((sumR / sumA) << 16) | ((sumG / sumA) << 8) | (sumB / sumA);
			img.propSource[static_cast<size_t>(ly) * img.loW + lx] = nearestOf(rgb);
		}
	}
	AddBytes(img, img.propSource.size());
}

} // namespace

void DrawProp(const Surface &out, const Prop &prop, Point pillarTopLeft, const uint8_t *levels, const uint8_t *lightTables, int numLevels,
    const uint8_t (*blend)[256])
{
	if (!Active || out.surface == nullptr || levels == nullptr || lightTables == nullptr || numLevels <= 0) return;
	uint32_t image = 0;
	{
		const std::lock_guard<std::mutex> lock(ImagesMutex);
		image = ImageId(prop.file, prop.w, prop.h);
		if (image != 0 && Images[image].w == 0) image = 0;
		if (image == 0) return;
		if (TileHeld.insert(image).second) Hold(image); // the tileset holds its props as it holds its pillars' art
		if (Images[image].propSource.empty()) PropSource(Images[image]);
	}
	const HdImage &img = Images[image];
	const int left = out.region.x + pillarTopLeft.x + prop.dx;
	const int top = out.region.y + pillarTopLeft.y + prop.dy;
	const int x0 = std::max({ left, out.region.x, 0 });
	const int y0 = std::max({ top, out.region.y, 0 });
	const int x1 = std::min({ left + img.loW, out.region.x + out.region.w, out.surface->w });
	const int y1 = std::min({ top + img.loH, out.region.y + out.region.h, out.surface->h });
	if (x0 >= x1 || y0 >= y1) return;
	const bool tracked = out.surface == Target && EnsurePlane();
	if (tracked) {
		if (BehindProv.size() != Plane.size()) {
			BehindProv.assign(Plane.size(), PixelProvenance {});
			BehindIdx.assign(Plane.size(), 0);
		}
		if (blend != nullptr && BlendLit.size() != Plane.size()) BlendLit.assign(Plane.size(), 0);
	}
	const uint32_t serial = tracked ? ++NextSerial : 0;
	const uint8_t shade = PixelShading ? ShadePixel : ShadeLevel;
	auto *pixels = static_cast<uint8_t *>(out.surface->pixels);
	const int pitch = out.surface->pitch;
	for (int y = y0; y < y1; ++y) {
		const uint8_t *sources = &img.propSource[static_cast<size_t>(y - top) * img.loW];
		for (int x = x0; x < x1; ++x) {
			const uint8_t source = sources[x - left];
			if (source == 0) continue;
			const int level = std::min<int>(levels[x - left], numLevels - 1);
			const uint8_t lit = lightTables[static_cast<size_t>(level) * 256 + source];
			const size_t at = static_cast<size_t>(y) * pitch + x;
			const uint8_t written = blend != nullptr ? blend[pixels[at]][lit] : lit;
			if (tracked) {
				// what was behind the pixel stays to show where the art is transparent (as under a free silhouette), or half behind it
				PixelProvenance behind = Plane[at];
				uint8_t index = pixels[at];
				if (behind.blend != 0) { // a blended pixel behind: its art as drawn opaque (one layer deep), as KeepBehind keeps it
					if (behind.image != 0 && behind.written == index && at < BlendLit.size()) index = behind.written = BlendLit[at];
					behind.blend = 0;
				}
				BehindProv[at] = behind;
				BehindIdx[at] = index;
				// free == 2: foliage drawn later goes under it, and it stays out of its light level's grade (which it takes)
				Plane[at] = { image, static_cast<int16_t>(left), static_cast<int16_t>(top), source, written, 2, 0, shade,
					static_cast<uint8_t>(std::min(level, 15)), static_cast<uint8_t>(blend != nullptr ? 1 : 0), serial };
				if (blend != nullptr) BlendLit[at] = lit;
			}
			pixels[at] = written;
		}
	}
}

bool InFloorDiamond(int x, int k)
{
	if (x < 0 || x >= 64 || k < 0 || k > 30) return false;
	if (x < 32) return k < 16 ? x >= 30 - 2 * k : x >= 2 * (k - 15);
	const int r = x - 32;
	return k < 16 ? r < 2 * k + 2 : r < 32 - 2 * (k - 15);
}

FloorOnlyScope::FloorOnlyScope(const Surface &out, Point bottomLeft, const uint8_t *tbl)
    : out_(out)
    , x0_(out.region.x + bottomLeft.x)
    , y0_(out.region.y + bottomLeft.y - 31)
    , tbl_(tbl)
    , image_(TrackImage)
    , serial_(TrackSerial)
    , left_(TrackLeft)
    , top_(TrackTop)
{
	if (out.surface == nullptr) return;
	const bool tracked = Active && out.surface == Target && EnsurePlane() && TrackPlane == Plane.data();
	idx_.resize(64 * 32);
	if (tracked) prov_.resize(64 * 32);
	const auto *pixels = static_cast<const uint8_t *>(out.surface->pixels);
	for (int j = 0; j < 32; ++j) {
		const int y = y0_ + j;
		if (y < 0 || y >= out.surface->h) continue;
		for (int i = 0; i < 64; ++i) {
			const int x = x0_ + i;
			if (x < 0 || x >= out.surface->w) continue;
			const size_t at = static_cast<size_t>(y) * out.surface->pitch + x;
			idx_[j * 64 + i] = pixels[at];
			if (tracked) prov_[j * 64 + i] = Plane[at];
		}
	}
}

FloorOnlyScope::~FloorOnlyScope()
{
	if (out_.surface == nullptr || idx_.empty()) return;
	const bool tracked = !prov_.empty() && out_.surface == Target && Plane.size() == static_cast<size_t>(PlanePitch) * PlaneHeight && PlanePitch == out_.surface->pitch;
	auto *pixels = static_cast<uint8_t *>(out_.surface->pixels);
	const HdImage *art = nullptr;
	if (tracked && tbl_ != nullptr && image_ != 0) {
		const std::lock_guard<std::mutex> lock(ImagesMutex);
		HdImage &img = Images[image_];
		if (img.resident && img.w != 0) {
			if (img.propSource.empty()) PropSource(img); // (the pillar's art, as a prop's: the reference colours nearest it)
			art = &img;
		}
	}
	const uint8_t light = LightLevelOf(tbl_);
	const uint8_t shade = PixelShading ? ShadePixel : ShadeLevel;
	// the diamond's pixels the pillar's own pieces left undrawn, from its art (left to right within the region: x0_.., the region of out_)
	const int rx0 = std::max(out_.region.x, 0), ry0 = std::max(out_.region.y, 0);
	const int rx1 = std::min(out_.region.x + out_.region.w, out_.surface->w), ry1 = std::min(out_.region.y + out_.region.h, out_.surface->h);
	for (int j = 0; art != nullptr && j < 32; ++j) {
		const int y = y0_ + j;
		if (y < ry0 || y >= ry1) continue;
		const int sy = y - top_;
		if (sy < 0 || sy >= art->loH) continue;
		for (int i = 0; i < 64; ++i) {
			const int x = x0_ + i;
			if (x < rx0 || x >= rx1 || !InFloorDiamond(i, 31 - j)) continue;
			const size_t at = static_cast<size_t>(y) * out_.surface->pitch + x;
			if (Plane[at].serial == serial_) continue; // drawn by its pieces
			const int sx = x - left_;
			if (sx < 0 || sx >= art->loW) continue;
			const uint8_t source = art->propSource[static_cast<size_t>(sy) * art->loW + sx];
			if (source == 0) continue;
			pixels[at] = tbl_[source];
			Plane[at] = { image_, left_, top_, source, tbl_[source], 0, 0, shade, light, 0, serial_ };
		}
	}
	for (int j = 0; j < 32; ++j) {
		const int y = y0_ + j;
		if (y < 0 || y >= out_.surface->h) continue;
		for (int i = 0; i < 64; ++i) {
			const int x = x0_ + i;
			if (x < 0 || x >= out_.surface->w || InFloorDiamond(i, 31 - j)) continue;
			const size_t at = static_cast<size_t>(y) * out_.surface->pitch + x;
			pixels[at] = idx_[j * 64 + i];
			if (tracked) Plane[at] = prov_[j * 64 + i];
		}
	}
}

void RequestDump(std::string_view name)
{
	if (Active && !DumpDir.empty()) RequestedDump = name;
}

bool Present(SDL_Renderer *renderer, SDL_Surface *palSurface, int dumpStep)
{
	if (!Active || palSurface == nullptr || palSurface->format->palette == nullptr) return false;
	Target = palSurface;
	Collect();
	const auto composeStart = std::chrono::steady_clock::now();
	Compose(palSurface);
	ComposeMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - composeStart).count();
	Dump(palSurface, dumpStep);
	++PresentCount;
	++UseClock;
	if (!MemLog.empty() && PresentCount % 300 == 0) {
		const MemoryUse use = Memory();
		std::ofstream(MemLog, std::ios::app) << "present " << PresentCount << " bytes " << use.bytes << " peak " << use.peak << " images " << use.images
		                                     << " indexed " << use.indexed << " loads " << use.loads << " unloads " << use.unloads << " composeMs " << ComposeMs
		                                     << " loadMs " << LoadMicros / 1000.0 << " loadMaxMs " << LoadMaxMicros / 1000.0 << '\n';
	}
	if (renderer == nullptr) return false;
	const int w = palSurface->w * Scale;
	const int h = palSurface->h * Scale;
	if (FrameTexture == nullptr || TextureW != w || TextureH != h) {
		if (FrameTexture != nullptr) SDL_DestroyTexture(FrameTexture);
		FrameTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
		TextureW = w;
		TextureH = h;
		if (FrameTexture == nullptr) {
			LogError("HD: can't create a {}x{} texture: {}", w, h, SDL_GetError());
			Active = false;
			return false;
		}
	}
	SDL_UpdateTexture(FrameTexture, nullptr, Frame.data(), w * 4);
	SDL_RenderCopy(renderer, FrameTexture, nullptr, nullptr);
	return true;
}

} // namespace devilution::hd
