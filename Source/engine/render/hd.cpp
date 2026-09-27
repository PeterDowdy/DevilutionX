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
};

struct SpriteEntry {
	std::string key; ///< "sprite/<path>/<group>.<frame>.png"
	uint32_t check;
	std::vector<uint8_t> original; ///< pixel data before ClxApplyTrans, when it was recoloured
	std::array<uint8_t, 256> trn;
};

bool Active = false;
std::string PackDir;
int Scale = 4;
std::unordered_map<std::string, std::string> Manifest; ///< image file -> palette file
std::vector<HdImage> Images(1);                         ///< id 0: none
std::unordered_map<std::string, uint32_t> ImageIds;     ///< image file -> id (0: no image)
std::unordered_map<const uint8_t *, SpriteEntry> Sprites;
std::string Tileset;

SDL_Surface *Target = nullptr; ///< the PalSurface
std::vector<PixelProvenance> Plane;
int PlanePitch = 0;
int PlaneHeight = 0;

std::vector<uint32_t> Frame;
SDL_Texture *FrameTexture = nullptr;
int TextureW = 0;
int TextureH = 0;

std::string DumpDir;
int DumpEvery = 0;
bool DumpIds = false; ///< also dump which image each pixel came from (ids-*.png, and ids.txt: id and file)
int PresentCount = 0;

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
	}
	return true;
}

struct SavedTrack {
	PixelProvenance *plane;
	const uint8_t *base;
	uint32_t image;
	int16_t left, top;
};
SavedTrack Saved;

void Begin(const Surface &out, uint32_t image, int left, int top)
{
	Saved = { TrackPlane, TrackBase, TrackImage, TrackLeft, TrackTop };
	if (!Active || out.surface != Target || !EnsurePlane()) return;
	TrackPlane = Plane.data();
	TrackBase = static_cast<const uint8_t *>(Target->pixels);
	TrackImage = image;
	TrackLeft = static_cast<int16_t>(left);
	TrackTop = static_cast<int16_t>(top);
}

void End()
{
	TrackPlane = Saved.plane;
	TrackBase = Saved.base;
	TrackImage = Saved.image;
	TrackLeft = Saved.left;
	TrackTop = Saved.top;
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

void ComposeRows(SDL_Surface *pal, int y0, int y1)
{
	const int w = pal->w;
	const int n = Scale;
	const int outW = w * n;
	const SDL_Color *live = pal->format->palette->colors;
	const auto *pixels = static_cast<const uint8_t *>(pal->pixels);
	const bool havePlane = PlanePitch == pal->pitch && PlaneHeight == pal->h;
	int us[8];
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
			const int sx = x - p->left;
			const int sy = y - p->top;
			if (sx < 0 || sy < 0 || sx >= img->loW || sy >= img->loH) {
				FillBlock(dst, outW, n, flat);
				continue;
			}
			const bool graded = img->hasRef;
			const uint8_t *ref = &img->ref[p->source * 3];
			const ChannelGrade gr = graded ? ChannelGrade(ref[0], c.r) : ChannelGrade(0, 0);
			const ChannelGrade gg = graded ? ChannelGrade(ref[1], c.g) : ChannelGrade(0, 0);
			const ChannelGrade gb = graded ? ChannelGrade(ref[2], c.b) : ChannelGrade(0, 0);
			for (int i = 0; i < n; ++i)
				us[i] = std::min(img->w - 1, (sx * n + i) * img->w / (img->loW * n));
			for (int j = 0; j < n; ++j) {
				const int v = std::min(img->h - 1, (sy * n + j) * img->h / (img->loH * n));
				const uint32_t *row = &img->px[static_cast<size_t>(v) * img->w];
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
					if (a < 255) { // blend over the pixel's own colour
						r = (r * a + c.r * (255 - a)) / 255;
						g = (g * a + c.g * (255 - a)) / 255;
						b = (b * a + c.b * (255 - a)) / 255;
					}
					out[i] = 0xFF000000U | (r << 16) | (g << 8) | b;
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
}

void Dump(SDL_Surface *pal)
{
	if (DumpDir.empty() || DumpEvery <= 0 || PresentCount % DumpEvery != 0) return;
	char name[32];
	std::snprintf(name, sizeof(name), "%05d.png", PresentCount / DumpEvery);
	SDL_Surface *hdSurface = SDL_CreateRGBSurfaceWithFormatFrom(Frame.data(), pal->w * Scale, pal->h * Scale, 32,
	    pal->w * Scale * 4, SDL_PIXELFORMAT_ARGB8888);
	if (hdSurface != nullptr) {
		IMG_SavePNG(hdSurface, (DumpDir + "/hd-" + name).c_str());
		SDL_FreeSurface(hdSurface);
	}
	IMG_SavePNG(pal, (DumpDir + "/low-" + name).c_str());
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
			IMG_SavePNG(idSurface, (DumpDir + "/ids-" + name).c_str());
			SDL_FreeSurface(idSurface);
		}
	}
}

} // namespace

bool Enabled()
{
	return Active;
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
	while (std::getline(index, line)) {
		std::istringstream words(line);
		std::string kind;
		words >> kind;
		if (kind == "scale") {
			words >> Scale;
		} else if (kind == "image") {
			std::string file, palette;
			words >> file >> palette;
			Manifest[file] = palette == "-" ? "" : palette;
		}
	}
	Scale = std::clamp(Scale, 1, 8);
	if (const char *dump = std::getenv("DEVILUTIONX_HD_DUMP"); dump != nullptr) {
		DumpDir = dump;
		const char *every = std::getenv("DEVILUTIONX_HD_DUMP_EVERY");
		DumpEvery = every != nullptr ? std::max(1, std::atoi(every)) : 60;
		DumpIds = std::getenv("DEVILUTIONX_HD_DUMP_IDS") != nullptr;
	}
	Active = true;
	LogInfo("HD: pack {} ({} images, {}x)", PackDir, Manifest.size(), Scale);
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
	const std::string stem = "sprite/" + NormalizePath(path) + "/" + std::to_string(group) + ".";
	for (uint32_t f = 0; f < list.numSprites(); ++f) {
		std::string key = stem + std::to_string(f) + ".png";
		const ClxSprite sprite = list[f];
		if (Manifest.count(key) != 0)
			Sprites[sprite.pixelData()] = { std::move(key), Checksum(sprite) };
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
	if (found != Sprites.end() && !found->second.original.empty())
		found->second.check = Checksum(sprite);
}

void SetTarget(SDL_Surface *palSurface)
{
	Target = palSurface;
}

void SetTileset(std::string_view name)
{
	Tileset = name;
}

SpriteScope::SpriteScope(const Surface &out, Point position, ClxSprite clx)
{
	uint32_t image = 0;
	if (Active) {
		const auto found = Sprites.find(clx.pixelData());
		if (found != Sprites.end() && found->second.check == Checksum(clx)) {
			image = ImageId(found->second.key, clx.width(), clx.height());
			if (image != 0 && !found->second.original.empty()) {
				original = found->second.original.data();
				trn = found->second.trn.data();
			}
		}
	}
	Begin(out, image, out.region.x + position.x, out.region.y + position.y - clx.height() + 1);
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

bool Present(SDL_Renderer *renderer, SDL_Surface *palSurface)
{
	if (!Active || palSurface == nullptr || palSurface->format->palette == nullptr) return false;
	Target = palSurface;
	Compose(palSurface);
	Dump(palSurface);
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
