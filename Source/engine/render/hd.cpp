#include "engine/render/hd.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

PixelProvenance *TrackPlane = nullptr;
const uint8_t *TrackBase = nullptr;
uint32_t TrackImage = 0;
int16_t TrackLeft = 0;
int16_t TrackTop = 0;
uint8_t TrackFree = 0;
uint32_t TrackSerial = 0;

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
std::vector<HdImage> Images(1);                         ///< id 0: none
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
	SDL_Surface *loaded = IMG_LoadPNG(path.c_str());
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

bool EnsurePlane()
{
	if (Target == nullptr) return false;
	if (PlanePitch != Target->pitch || PlaneHeight != Target->h) {
		PlanePitch = Target->pitch;
		PlaneHeight = Target->h;
		Plane.assign(static_cast<size_t>(PlanePitch) * PlaneHeight, PixelProvenance {});
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
SavedTrack Saved;

/** The draw in progress, when its art has a free silhouette: where it may draw (the art's rectangle, clipped to the surface). */
struct FreeDraw {
	bool on = false;
	uint32_t image = 0;
	int left = 0, top = 0;
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
FreeDraw Free;

void Begin(const Surface &out, uint32_t image, int left, int top, bool free = false)
{
	Saved = { TrackPlane, TrackBase, TrackImage, TrackLeft, TrackTop, TrackFree, TrackSerial };
	Free.on = false;
	if (!Active || out.surface != Target || !EnsurePlane()) return;
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
 * Draws the art's pixels outside the sprite's footprint into the PalSurface: each takes the index, and so the lighting,
 * of the nearest pixel the sprite drew (preferring one that isn't near black), and is recorded as the sprite's own.
 */
void ExtendFootprint()
{
	HdImage &img = Images[Free.image];
	if (img.reach.empty()) ComputeReach(img);
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
		Plane[extend[i].at] = { Free.image, static_cast<int16_t>(Free.left), static_cast<int16_t>(Free.top), donors[i].source, donors[i].written, 1, TrackSerial };
	}
}

void End()
{
	if (Free.on) {
		ExtendFootprint();
		Free.on = false;
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
	for (const auto &[file, palette] : Manifest) {
		if (file.rfind("sprite/", 0) != 0) continue;
		std::string stem = file.substr(0, file.rfind('/'));
		const size_t swap = stem.find('@', stem.rfind('/'));
		if (swap != std::string::npos) stem.resize(swap);
		ArtStems.insert(std::move(stem));
	}
	if (const char *dump = std::getenv("DEVILUTIONX_HD_DUMP"); dump != nullptr) {
		DumpDir = dump;
		const char *every = std::getenv("DEVILUTIONX_HD_DUMP_EVERY");
		DumpEvery = every != nullptr ? std::max(0, std::atoi(every)) : 60;
		DumpIds = std::getenv("DEVILUTIONX_HD_DUMP_IDS") != nullptr;
	}
	Active = true;
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

void SetTileset(std::string_view name)
{
	Tileset = name;
}

SpriteScope::SpriteScope(const Surface &out, Point position, ClxSprite clx, const uint8_t *drawTrn)
{
	uint32_t image = 0;
	if (Active) {
		const auto found = Sprites.find(clx.pixelData());
		if (found != Sprites.end() && found->second.check == Checksum(clx)) {
			const SpriteEntry &entry = found->second;
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
