#include "engine/render/hd.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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
	std::vector<uint32_t> px;     ///< 0xAARRGGBB
	std::array<uint8_t, 768> ref; ///< the palette the art was authored against
	bool hasRef = false;
	std::vector<uint8_t> reach; ///< per original pixel: the art is opaque somewhere in its block (free silhouettes; made when first needed)
	uint32_t fx = 0;            ///< a sprite frame's effect layer ("fx/..."), an image of its own; looked up when the frame is first drawn into the game view
	bool fxLooked = false;
	int fxDx = 0; ///< an effect layer: where its rectangle (loW x loH original pixels) starts, relative to its sprite's top-left
	int fxDy = 0;
};

struct SpriteEntry {
	std::string stem;  ///< "sprite/<path>"
	std::string frame; ///< "/<group>.<frame>.png"
	uint32_t check;
	std::string recolour;          ///< the named TRN ClxApplyTrans applied ("bluered"), if any
	bool swapArt = false;          ///< the pack has this frame's "@<recolour>" art, which already shows it
	bool pending = false;          ///< between BeforeRecolour and AfterRecolour
	std::vector<uint8_t> original; ///< pixel data before ClxApplyTrans, when its own art is graded through it
	std::array<uint8_t, 256> trn;

	/** The pack file of this frame in a swap ("bluered", "bluered+rcrn", "+eth"), or itself for none. */
	[[nodiscard]] std::string Key(std::string_view swap) const
	{
		return swap.empty() ? stem + frame : stem + "@" + std::string(swap) + frame;
	}
};

struct NamedTrn {
	std::string name;
	uint32_t check;
};

bool Active = false;
std::string PackDir;
int Scale = 4;
std::unordered_map<std::string, std::string> Manifest; ///< image file -> palette file
std::vector<HdImage> Images(1);                         ///< id 0: none; reserved for the whole manifest when the pack loads, so it never
                                                        ///< reallocates while Compose reads it (the level loader thread adds images)
std::mutex ImagesMutex;                                 ///< ImageId: the main and the loader thread look up / add images
std::unordered_map<std::string, uint32_t> ImageIds;     ///< image file -> id (0: no image)
std::unordered_map<const uint8_t *, SpriteEntry> Sprites;
std::unordered_set<std::string> ArtStems; ///< "sprite/<path>" with art for any frame, its own or a swap's
std::string RecolourName;                 ///< the TRN ClxApplyTrans is applying, while a NamedRecolour lives
std::unordered_map<const uint8_t *, NamedTrn> TrnNames;
std::string Tileset;

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
uint32_t NextSerial = 0;

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

/** The name of a TRN that draws pass, when it was named and hasn't changed since. */
std::string_view TrnName(const uint8_t *trn)
{
	if (trn == nullptr) return {};
	const auto found = TrnNames.find(trn);
	if (found == TrnNames.end() || found->second.check != TrnChecksum(trn)) return {};
	return found->second.name;
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

bool LoadImage(const std::string &file, const std::string &palette, HdImage &img)
{
	const std::string path = PackDir + "/" + file;
	// open and close the file here: IMG_LoadPNG(path) leaves its file open, one descriptor per HD image, until the process
	// runs out of them (Peter, 2026-11-01: crashed in the dungeon with "Too many open files")
#ifdef USE_SDL3
	SDL_IOStream *src = SDL_IOFromFile(path.c_str(), "rb");
	SDL_Surface *loaded = src != nullptr ? IMG_LoadPNG_IO(src) : nullptr;
	if (src != nullptr) SDL_CloseIO(src);
#else
	SDL_RWops *src = SDL_RWFromFile(path.c_str(), "rb");
	SDL_Surface *loaded = src != nullptr ? IMG_LoadPNG_RW(src) : nullptr;
	if (src != nullptr) SDL_RWclose(src);
#endif
	if (loaded == nullptr) {
		LogError("HD: can't load {}: {}", path, SDL_GetError());
		return false;
	}
	SDL_Surface *argb = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_ARGB8888, 0);
	SDL_FreeSurface(loaded);
	if (argb == nullptr) return false;
	img.w = argb->w;
	img.h = argb->h;
	img.px.resize(static_cast<size_t>(img.w) * img.h);
	for (int y = 0; y < img.h; ++y)
		std::memcpy(&img.px[static_cast<size_t>(y) * img.w], static_cast<uint8_t *>(argb->pixels) + static_cast<size_t>(y) * argb->pitch, img.w * 4);
	SDL_FreeSurface(argb);
	if (!palette.empty()) {
		std::ifstream pal(PackDir + "/" + palette, std::ios::binary);
		img.hasRef = static_cast<bool>(pal.read(reinterpret_cast<char *>(img.ref.data()), img.ref.size()));
	}
	return true;
}

uint32_t ImageId(const std::string &file, int loW, int loH)
{
	const std::lock_guard<std::mutex> lock(ImagesMutex);
	const auto found = ImageIds.find(file);
	if (found != ImageIds.end()) return found->second;
	uint32_t id = 0;
	const auto entry = Manifest.find(file);
	if (entry != Manifest.end()) {
		HdImage img;
		if (LoadImage(file, entry->second, img)) {
			img.loW = loW;
			img.loH = loH;
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
 * "sprite/<path>/<group>.<frame>.png". 0 when the pack has none, or the file doesn't say where it goes.
 */
uint32_t FxOf(uint32_t image, const SpriteEntry &entry)
{
	if (Images[image].fxLooked) return Images[image].fx;
	Images[image].fxLooked = true;
	const std::string file = "fx" + entry.Key({}).substr(std::strlen("sprite"));
	if (Manifest.count(file) == 0) return 0;
	int dx = 0, dy = 0, w = 0, h = 0;
	std::istringstream words(ReadPngText(PackDir + "/" + file, "hdfx"));
	if (!(words >> dx >> dy >> w >> h) || w <= 0 || h <= 0) {
		LogError("HD: {} doesn't say where it goes (tEXt hdfx)", file);
		return 0;
	}
	const uint32_t fx = ImageId(file, w, h);
	if (fx == 0 || Images[fx].w < 2) return 0;
	Images[fx].fxDx = dx;
	Images[fx].fxDy = dy;
	Images[image].fx = fx;
	return fx;
}

bool EnsurePlane()
{
	if (Target == nullptr) return false;
	if (PlanePitch != Target->pitch || PlaneHeight != Target->h) {
		PlanePitch = Target->pitch;
		PlaneHeight = Target->h;
		Plane.assign(static_cast<size_t>(PlanePitch) * PlaneHeight, PixelProvenance {});
		Overlays.clear();
		if (FreeSilhouette) {
			BehindProv.assign(Plane.size(), PixelProvenance {});
			BehindIdx.assign(Plane.size(), 0);
		}
	}
	return true;
}

struct SavedTrack {
	PixelProvenance *plane;
	const uint8_t *base;
	uint32_t image;
	int16_t left, top;
	uint8_t free;
	uint32_t serial;
};
thread_local SavedTrack Saved;

/** The draw in progress, when its art has a free silhouette: where it may draw (the art's rectangle, clipped to the surface). */
struct FreeDraw {
	bool on = false;
	uint32_t image = 0;
	int left = 0, top = 0;
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	bool whole = false; ///< no limit on how far from the sprite's own pixels its art is drawn (art with an effect layer)
};
thread_local FreeDraw Free;

/** The sprite draw in progress, when its art has an effect layer: End adds it to Overlays. */
struct FxDraw {
	uint32_t fx = 0;
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
	Saved = { TrackPlane, TrackBase, TrackImage, TrackLeft, TrackTop, TrackFree, TrackSerial };
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
	if (!free || !FreeSilhouette || image == 0 || Images[image].w == 0 || BehindProv.size() != Plane.size()) return;
	const HdImage &img = Images[image];
	const int x0 = std::max({ left, out.region.x, 0 });
	const int y0 = std::max({ top, out.region.y, 0 });
	const int x1 = std::min({ left + img.loW, out.region.x + out.region.w, Target->w });
	const int y1 = std::min({ top + img.loH, out.region.y + out.region.h, Target->h });
	if (x0 >= x1 || y0 >= y1) return;
	Free = { true, image, left, top, x0, y0, x1, y1 };
	TrackFree = 1;
	const auto *pixels = static_cast<const uint8_t *>(Target->pixels);
	for (int y = y0; y < y1; ++y) {
		const size_t at = static_cast<size_t>(y) * PlanePitch + x0;
		std::memcpy(&BehindIdx[at], pixels + at, x1 - x0);
		std::copy_n(&Plane[at], x1 - x0, &BehindProv[at]);
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
					best = std::max(best, img.px[static_cast<size_t>(v) * img.w + u] >> 24);
				}
			}
			img.reach[static_cast<size_t>(ly) * img.loW + lx] = best >= 32 ? 1 : 0;
		}
	}
}

/**
 * ExtendFootprint for art with an effect layer (a spell cast): no limit on how far from the sprite's own pixels the art is drawn, since the
 * pose of a cast is not bound to the original's. Two passes over the art's rectangle give each pixel its nearest drawn pixel by chessboard
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
		Plane[e.at] = { Free.image, static_cast<int16_t>(Free.left), static_cast<int16_t>(Free.top), e.donor.source, e.donor.written, 2, 0, TrackSerial };
	}
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
			for (int r = 1; r <= Radius && r < bestScore; ++r) {
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
		Plane[extend[i].at] = { Free.image, static_cast<int16_t>(Free.left), static_cast<int16_t>(Free.top), donors[i].source, donors[i].written, 2, 0, TrackSerial };
	}
}

/** The sprite just drawn has an effect layer: composed over the frame from now on, where the sprite's surface lets it. */
void AddOverlay()
{
	const HdImage &img = Images[Fx.fx];
	Overlay o { Fx.fx, TrackLeft + img.fxDx, TrackTop + img.fxDy, 0, 0, 0, 0, TrackSerial, {} };
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

void End()
{
	if (Free.on) {
		ExtendFootprint();
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
	TrackSerial = Saved.serial;
}

/**
 * How one channel of an HD pixel follows its 8-bit pixel: times live / ref (16.16 fixed point, rounded up
 * so that ref itself maps exactly to live), or plus live - ref when ref is too dark for a ratio.
 */
struct ChannelGrade {
	int32_t factor; ///< 0: use the offset
	int32_t offset;

	ChannelGrade(uint8_t ref, uint8_t live)
	    : factor(ref >= 16 ? static_cast<int32_t>(((live << 16) + ref - 1) / ref) : 0)
	    , offset(live - ref)
	{
	}

	[[nodiscard]] uint32_t apply(uint32_t c) const
	{
		const int32_t v = factor != 0 ? static_cast<int32_t>((c * factor) >> 16) : static_cast<int32_t>(c) + offset;
		return static_cast<uint32_t>(std::clamp(v, 0, 255));
	}
};

void FillBlock(uint32_t *dst, int outW, int n, uint32_t color)
{
	for (int j = 0; j < n; ++j)
		std::fill_n(dst + static_cast<size_t>(j) * outW, n, color);
}

/**
 * Draws the HD block of the pixel (``x``, ``y``), live colour ``c``, from ``img`` into ``dst`` (n rows of n, ``outW`` apart), graded
 * by the live colour. Where the art is not opaque it blends over ``under`` (n x n, packed) or, without it, over ``c``.
 * False when the pixel is outside the art.
 */
bool SampleBlock(const HdImage &img, const PixelProvenance &p, int x, int y, SDL_Color c, uint32_t *dst, int outW, const uint32_t *under)
{
	const int n = Scale;
	const int sx = x - p.left;
	const int sy = y - p.top;
	if (sx < 0 || sy < 0 || sx >= img.loW || sy >= img.loH) return false;
	const bool graded = img.hasRef;
	const uint8_t *ref = &img.ref[p.source * 3];
	const ChannelGrade gr = graded ? ChannelGrade(ref[0], c.r) : ChannelGrade(0, 0);
	const ChannelGrade gg = graded ? ChannelGrade(ref[1], c.g) : ChannelGrade(0, 0);
	const ChannelGrade gb = graded ? ChannelGrade(ref[2], c.b) : ChannelGrade(0, 0);
	int us[8];
	for (int i = 0; i < n; ++i)
		us[i] = std::min(img.w - 1, (sx * n + i) * img.w / (img.loW * n));
	for (int j = 0; j < n; ++j) {
		const int v = std::min(img.h - 1, (sy * n + j) * img.h / (img.loH * n));
		const uint32_t *row = &img.px[static_cast<size_t>(v) * img.w];
		uint32_t *out = dst + static_cast<size_t>(j) * outW;
		for (int i = 0; i < n; ++i) {
			const uint32_t s = row[us[i]];
			const uint32_t a = s >> 24;
			uint32_t r = (s >> 16) & 0xFF, g = (s >> 8) & 0xFF, b = s & 0xFF;
			if (graded) {
				r = gr.apply(r);
				g = gg.apply(g);
				b = gb.apply(b);
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
	for (int y = y0; y < y1; ++y) {
		for (int x = 0; x < w; ++x) {
			const size_t at = static_cast<size_t>(y) * pal->pitch + x;
			const uint8_t index = pixels[at];
			const SDL_Color c = live[index];
			const uint32_t flat = 0xFF000000U | (c.r << 16) | (c.g << 8) | c.b;
			uint32_t *dst = &Frame[static_cast<size_t>(y) * n * outW + static_cast<size_t>(x) * n];
			const PixelProvenance *p = havePlane ? &Plane[at] : nullptr;
			const HdImage *img = p != nullptr && p->image != 0 && p->written == index ? &Images[p->image] : nullptr;
			if (img == nullptr || img->w == 0) {
				FillBlock(dst, outW, n, flat);
				continue;
			}
			if (p->free != 0 && haveBehind) {
				// Free silhouette: the art blends over what was behind the sprite here, not over the sprite's own pixel
				uint32_t under[64];
				const uint8_t behind = BehindIdx[at];
				const SDL_Color bc = live[behind];
				const PixelProvenance &bp = BehindProv[at];
				const HdImage *behindImg = bp.image != 0 && bp.written == behind ? &Images[bp.image] : nullptr;
				if (behindImg == nullptr || behindImg->w == 0 || !SampleBlock(*behindImg, bp, x, y, bc, under, n, nullptr))
					FillBlock(under, n, n, 0xFF000000U | (bc.r << 16) | (bc.g << 8) | bc.b);
				if (!SampleBlock(*img, *p, x, y, c, dst, outW, under)) FillBlock(dst, outW, n, flat);
				continue;
			}
			if (!SampleBlock(*img, *p, x, y, c, dst, outW, nullptr)) FillBlock(dst, outW, n, flat);
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
					const uint32_t *row = &img.px[static_cast<size_t>(v) * img.w];
					uint32_t *out = &Frame[(static_cast<size_t>(y) * n + j) * outW + static_cast<size_t>(x) * n];
					for (int i = 0; i < n; ++i) {
						const uint32_t over = row[us[i]];
						const uint32_t glow = row[half + us[i]] & 0xFFFFFFU;
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

void Compose(SDL_Surface *pal)
{
	Frame.resize(static_cast<size_t>(pal->w) * Scale * pal->h * Scale);
	const int threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1U, 8U));
	std::vector<std::thread> workers;
	const int rows = (pal->h + threads - 1) / threads;
	for (int t = 1; t < threads; ++t) {
		const int y0 = t * rows;
		if (y0 < pal->h) workers.emplace_back(ComposeRows, pal, y0, std::min(pal->h, y0 + rows));
	}
	ComposeRows(pal, 0, std::min(pal->h, rows));
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
				ids[static_cast<size_t>(y) * pal->w + x] = (static_cast<uint32_t>(p.source) << 24) | (p.written == pixels[static_cast<size_t>(y) * pal->pitch + x] ? p.image : 0);
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

int PackScale()
{
	const char *dir = std::getenv("DEVILUTIONX_HD_PACK");
	if (!Supported || dir == nullptr || *dir == '\0') return 1;
	std::ifstream index(std::string(dir) + "/hd.txt");
	std::string line;
	while (std::getline(index, line)) {
		std::istringstream words(line);
		std::string kind;
		int scale = 0;
		if (words >> kind >> scale && kind == "scale") return std::clamp(scale, 1, 8);
	}
	return 1;
}

void Init()
{
	const char *dir = std::getenv("DEVILUTIONX_HD_PACK");
	if (!Supported || dir == nullptr || *dir == '\0') return;
	PackDir = dir;
	std::ifstream index(PackDir + "/hd.txt");
	if (!index) {
		LogError("HD: no hd.txt in {}", PackDir);
		return;
	}
	std::string line;
	FreeSilhouette = false;
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
		} else if (kind == "image") {
			std::string file, palette;
			words >> file >> palette;
			Manifest[file] = palette == "-" ? "" : palette;
		}
	}
	Scale = std::clamp(Scale, 1, 8);
	Images.reserve(Manifest.size() + 1);
	for (const auto &[file, palette] : Manifest) {
		if (file.rfind("sprite/", 0) != 0) continue;
		std::string stem = file.substr(0, file.rfind('/'));
		const size_t swap = stem.find('@', stem.rfind('/'));
		if (swap != std::string::npos) {
			ArtStems.insert(stem); // files registered under an "@" name of their own: Hellfire's menu files ("ui_art/heros@hf", load_pcx.cpp)
			stem.resize(swap);
		}
		ArtStems.insert(std::move(stem));
	}
	if (const char *dump = std::getenv("DEVILUTIONX_HD_DUMP"); dump != nullptr) {
		DumpDir = dump;
		const char *every = std::getenv("DEVILUTIONX_HD_DUMP_EVERY");
		DumpEvery = every != nullptr ? std::max(0, std::atoi(every)) : 60;
		DumpIds = std::getenv("DEVILUTIONX_HD_DUMP_IDS") != nullptr;
	}
	Active = true;
	SurfaceBlitHook = &CopyProvenance;
	LogInfo("HD: pack {} ({} images, {}x{})", PackDir, Manifest.size(), Scale, FreeSilhouette ? ", free silhouettes" : "");
}

void RegisterSprites(std::string_view path, ClxSpriteListOrSheet sprites)
{
	if (!Active) return;
	if (sprites.isSheet()) {
		const ClxSpriteSheet sheet = sprites.sheet();
		for (uint16_t g = 0; g < sheet.numLists(); ++g)
			RegisterSpriteList(path, g, sheet[g]);
	} else {
		RegisterSpriteList(path, 0, sprites.list());
	}
}

void RegisterSpriteList(std::string_view path, uint16_t group, ClxSpriteList list)
{
	if (!Active) return;
	const std::string stem = "sprite/" + NormalizePath(path);
	const bool art = ArtStems.count(stem) != 0;
	for (uint32_t f = 0; f < list.numSprites(); ++f) {
		const ClxSprite sprite = list[f];
		if (art)
			Sprites[sprite.pixelData()] = { stem, "/" + std::to_string(group) + "." + std::to_string(f) + ".png", Checksum(sprite) };
		else
			Sprites.erase(sprite.pixelData()); // a buffer reused at the same address
	}
}

void BeforeRecolour(ClxSprite sprite, const uint8_t *trn)
{
	if (!Active) return;
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

void NameTrn(const uint8_t *trn, std::string_view name)
{
	if (!Active || trn == nullptr) return;
	TrnNames[trn] = { TrnStem(name), TrnChecksum(trn) };
}

void SetTarget(SDL_Surface *palSurface)
{
	Target = palSurface;
}

void TrackOffscreen(SDL_Surface *buffer)
{
	if (!Active || buffer == nullptr) return;
	Offscreen *off = FindOffscreen(buffer);
	if (off == nullptr) off = &Offscreens.emplace_back(Offscreen { buffer, {} });
	off->plane.assign(static_cast<size_t>(buffer->pitch) * buffer->h, PixelProvenance {});
}

void ForgetOffscreen(SDL_Surface *buffer)
{
	Offscreens.erase(std::remove_if(Offscreens.begin(), Offscreens.end(), [buffer](const Offscreen &o) { return o.surface == buffer; }), Offscreens.end());
}

void SetTileset(std::string_view name)
{
	Tileset = name;
}

void NewScene()
{
	Overlays.clear();
}

SpriteScope::SpriteScope(const Surface &out, Point position, ClxSprite clx, const uint8_t *drawTrn)
{
	uint32_t image = 0;
	const SpriteEntry *drawn = nullptr;
	if (Active) {
		const auto found = Sprites.find(clx.pixelData());
		if (found != Sprites.end() && found->second.check == Checksum(clx)) {
			const SpriteEntry &entry = found->second;
			drawn = &entry;
			// The most specific art the pack has: the draw's own swap on top of the sprite's, the sprite's swap, itself
			if (const std::string_view unique = TrnName(drawTrn); !unique.empty()) {
				image = ImageId(entry.Key(entry.recolour + "+" + std::string(unique)), clx.width(), clx.height());
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
		}
	}
	Begin(out, image, out.region.x + position.x, out.region.y + position.y - clx.height() + 1, true);
	if (image == 0 || out.surface != Target || TrackPlane != Plane.data() || TrackImage != image) return; // not tracked in the game's own surface
	if (const uint32_t fx = FxOf(image, *drawn); fx != 0) {
		Fx = { fx, std::max(out.region.x, 0), std::max(out.region.y, 0), std::min(out.region.x + out.region.w, Target->w), std::min(out.region.y + out.region.h, Target->h) };
		Free.whole = Free.on;
	}
}

SpriteScope::~SpriteScope()
{
	End();
}

PillarScope::PillarScope(const Surface &out, Point bottomLeft, uint16_t pillar, int rows)
{
	uint32_t image = 0;
	if (Active && !Tileset.empty())
		image = ImageId("tile/" + Tileset + "/" + std::to_string(pillar) + ".png", 64, rows * 32);
	Begin(out, image, out.region.x + bottomLeft.x, out.region.y + bottomLeft.y - rows * 32 + 1);
}

PillarScope::~PillarScope()
{
	End();
}

void RequestDump(std::string_view name)
{
	if (Active && !DumpDir.empty()) RequestedDump = name;
}

bool Present(SDL_Renderer *renderer, SDL_Surface *palSurface, int dumpStep)
{
	if (!Active || palSurface == nullptr || palSurface->format->palette == nullptr) return false;
	Target = palSurface;
	Compose(palSurface);
	Dump(palSurface, dumpStep);
	++PresentCount;
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
