#include "engine/render/hd.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <SDL.h>
#include <gtest/gtest.h>

#include "engine/clx_sprite.hpp"
#include "engine/render/clx_render.hpp"
#include "engine/surface.hpp"
#include "utils/png.h"

namespace devilution {
namespace {

namespace fs = std::filesystem;

// Palette indices: the sprites' own colour, a monster type's swap of it, and a unique monster's on top
constexpr uint8_t Own = 10;
constexpr uint8_t Swapped = 20;
constexpr uint8_t Unique = 30;

/** A CLX list of one 4x4 sprite, every pixel ``Own``. */
std::vector<uint8_t> Sprite()
{
	std::vector<uint8_t> data = { 1, 0, 0, 0, 12, 0, 0, 0, 38, 0, 0, 0, 6, 0, 4, 0, 4, 0 };
	for (int row = 0; row < 4; ++row)
		data.insert(data.end(), { 0xFC, Own, Own, Own, Own });
	return data;
}

std::array<SDL_Color, 256> Palette()
{
	std::array<SDL_Color, 256> colors;
	colors.fill({ 100, 100, 100, 255 });
	colors[Own] = { 200, 50, 50, 255 };
	colors[Swapped] = { 50, 50, 200, 255 };
	colors[Unique] = { 50, 200, 50, 255 };
	return colors;
}

/** One HD image, 8x8 of one colour. */
void WriteArt(const fs::path &pack, const std::string &file, SDL_Color c, std::ofstream &index)
{
	fs::create_directories((pack / file).parent_path());
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 32, SDL_PIXELFORMAT_ARGB8888);
	SDL_FillRect(s, nullptr, SDL_MapRGBA(s->format, c.r, c.g, c.b, 255));
	IMG_SavePNG(s, (pack / file).string().c_str());
	SDL_FreeSurface(s);
	index << "image " << file << " palettes/test.pal\n";
}

SDL_Color At(SDL_Surface *hd, int x, int y)
{
	SDL_Color c;
	SDL_GetRGB(static_cast<uint32_t *>(hd->pixels)[y * (hd->pitch / 4) + x], hd->format, &c.r, &c.g, &c.b);
	return c;
}

void ExpectColor(SDL_Color got, int r, int g, int b, const char *what)
{
	EXPECT_EQ(got.r, r) << what;
	EXPECT_EQ(got.g, g) << what;
	EXPECT_EQ(got.b, b) << what;
}

TEST(HdTest, DrawsTheArtOfEachPaletteSwap)
{
	const fs::path pack = fs::temp_directory_path() / "devilutionx_hd_test";
	const fs::path dump = pack / "dump";
	fs::remove_all(pack);
	fs::create_directories(dump);
	const std::array<SDL_Color, 256> colors = Palette();
	{
		fs::create_directories(pack / "palettes");
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\n";
		WriteArt(pack, "sprite/test/a/0.0.png", { 120, 60, 60 }, index);
		WriteArt(pack, "sprite/test/a@+u/0.0.png", { 60, 60, 120 }, index);
		WriteArt(pack, "sprite/test/b/0.0.png", { 120, 60, 60 }, index);
		WriteArt(pack, "sprite/test/b@swap/0.0.png", { 60, 120, 60 }, index);
		WriteArt(pack, "sprite/test/b@swap+u/0.0.png", { 120, 120, 60 }, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	ASSERT_TRUE(hd::Enabled());

	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 32, 8, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, 0);
	hd::SetTarget(pal);
	const Surface out(pal);

	std::array<uint8_t, 256> typeTrn, uniqueTrn, otherTrn;
	for (int i = 0; i < 256; ++i)
		typeTrn[i] = uniqueTrn[i] = otherTrn[i] = static_cast<uint8_t>(i);
	typeTrn[Own] = Swapped;
	otherTrn[Own] = Swapped;
	uniqueTrn[Own] = Swapped;
	uniqueTrn[Swapped] = Unique;
	hd::NameTrn(uniqueTrn.data(), "Monsters\\Monsters\\U");

	std::vector<uint8_t> a = Sprite();
	std::vector<uint8_t> b = Sprite();
	hd::RegisterSpriteList("Test\\A.CL2", 0, ClxSpriteList { a.data() });
	hd::RegisterSpriteList("Test\\B.CL2", 0, ClxSpriteList { b.data() });
	{
		const hd::NamedRecolour swap("Test\\Swap"); // as InitMonsterTRN applies a type's TRN
		ClxApplyTrans(ClxSpriteList { b.data() }, typeTrn.data());
	}

	ClxDrawTRN(out, { 0, 3 }, ClxSpriteList { a.data() }[0], otherTrn.data()); // an unnamed TRN: graded
	ClxDrawTRN(out, { 8, 3 }, ClxSpriteList { a.data() }[0], uniqueTrn.data());
	ClxDraw(out, { 16, 3 }, ClxSpriteList { b.data() }[0]);
	ClxDrawTRN(out, { 24, 3 }, ClxSpriteList { b.data() }[0], uniqueTrn.data());
	hd::Present(nullptr, pal);

	SDL_Surface *hd = IMG_LoadPNG((dump / "hd-00000.png").string().c_str());
	ASSERT_NE(hd, nullptr);
	SDL_Surface *argb = SDL_ConvertSurfaceFormat(hd, SDL_PIXELFORMAT_ARGB8888, 0);
	// Own art graded by live[Swapped] / ref[Own], as before swaps had art
	ExpectColor(At(argb, 2, 2), 30, 60, 240, "a through an unnamed TRN");
	// A swap's art shows it already: drawn as it is, so its pixels must record the swapped indices
	ExpectColor(At(argb, 18, 2), 60, 60, 120, "a through the unique's TRN");
	ExpectColor(At(argb, 34, 2), 60, 120, 60, "b in its type's swap");
	ExpectColor(At(argb, 50, 2), 120, 120, 60, "b in its type's swap, through the unique's TRN");
	SDL_FreeSurface(argb);
	SDL_FreeSurface(hd);
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
}

/** A CLX list of one 4x4 sprite whose left column is transparent: the other three, every pixel ``Own``. */
std::vector<uint8_t> NarrowSprite()
{
	std::vector<uint8_t> data = { 1, 0, 0, 0, 12, 0, 0, 0, 38, 0, 0, 0, 6, 0, 4, 0, 4, 0 };
	for (int row = 0; row < 4; ++row)
		data.insert(data.end(), { 1, 0xFD, Own, Own, Own });
	return data;
}

/** One 8x8 HD image: opaque ``c`` everywhere except a transparent 2x2 hole at (2, 2) when ``hole``. */
void WriteArtWithHole(const fs::path &pack, const std::string &file, SDL_Color c, bool hole, std::ofstream &index)
{
	fs::create_directories((pack / file).parent_path());
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 32, SDL_PIXELFORMAT_ARGB8888);
	for (int y = 0; y < 8; ++y) {
		for (int x = 0; x < 8; ++x) {
			const bool clear = hole && x >= 2 && x < 4 && y >= 2 && y < 4;
			static_cast<uint32_t *>(s->pixels)[y * (s->pitch / 4) + x] = clear ? 0U : (0xFF000000U | (c.r << 16) | (c.g << 8) | c.b);
		}
	}
	IMG_SavePNG(s, (pack / file).string().c_str());
	SDL_FreeSurface(s);
	index << "image " << file << " palettes/test.pal\n";
}

SDL_Surface *LoadFrame(const fs::path &dump)
{
	for (const fs::directory_entry &entry : fs::directory_iterator(dump)) {
		if (entry.path().filename().string().rfind("hd-", 0) != 0) continue;
		SDL_Surface *loaded = IMG_LoadPNG(entry.path().string().c_str());
		if (loaded == nullptr) return nullptr;
		SDL_Surface *argb = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_ARGB8888, 0);
		SDL_FreeSurface(loaded);
		return argb;
	}
	return nullptr;
}

constexpr SDL_Color Background = { 100, 100, 100, 255 };
constexpr SDL_Color FrontArt = { 120, 60, 60, 255 };
constexpr SDL_Color BackArt = { 60, 120, 60, 255 };
constexpr uint8_t BackgroundIndex = 5;

/**
 * Draws, on a surface ``width`` wide, a sprite whose left column is transparent (at lo pixels x 8..11, y 4..7), and one over another
 * sprite with art of its own (x 16..19, y 4..7), in a pack that is ``free`` or not. The art of the first covers the whole sprite,
 * the art of the front sprite of the second has a hole over its pixel (17, 5). ``after`` runs before the frame is presented.
 * Returns the HD frame; the PalSurface comes back in ``palOut``.
 */
SDL_Surface *DrawFreeScenario(const std::string &name, bool free, int width, SDL_Surface **palOut, const std::function<void(SDL_Surface *)> &after = {})
{
	const fs::path pack = fs::temp_directory_path() / ("devilutionx_hd_" + name);
	const fs::path dump = pack / "dump";
	fs::remove_all(pack);
	fs::create_directories(dump);
	const std::array<SDL_Color, 256> colors = Palette();
	{
		fs::create_directories(pack / "palettes");
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\n";
		if (free) index << "silhouette free\n";
		WriteArtWithHole(pack, "sprite/" + name + "/a/0.0.png", FrontArt, false, index);
		WriteArtWithHole(pack, "sprite/" + name + "/back/0.0.png", BackArt, false, index);
		WriteArtWithHole(pack, "sprite/" + name + "/front/0.0.png", FrontArt, true, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	EXPECT_TRUE(hd::Enabled());

	// Not the size of any other test's surface, so that the provenance plane starts clean
	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, width, 12, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, BackgroundIndex);
	hd::SetTarget(pal);
	const Surface out(pal);

	// Sprites are found by the address of their pixel data: keep these alive and in place
	static std::vector<uint8_t> narrow, back, front;
	narrow = NarrowSprite();
	back = Sprite();
	front = Sprite();
	hd::RegisterSpriteList(name + "\\A.CL2", 0, ClxSpriteList { narrow.data() });
	hd::RegisterSpriteList(name + "\\Back.CL2", 0, ClxSpriteList { back.data() });
	hd::RegisterSpriteList(name + "\\Front.CL2", 0, ClxSpriteList { front.data() });
	ClxDraw(out, { 8, 7 }, ClxSpriteList { narrow.data() }[0]);
	ClxDraw(out, { 16, 7 }, ClxSpriteList { back.data() }[0]);
	ClxDraw(out, { 16, 7 }, ClxSpriteList { front.data() }[0]);
	if (after) after(pal);
	hd::Present(nullptr, pal);
	SDL_Surface *frame = LoadFrame(dump);
	fs::remove_all(pack);
	*palOut = pal;
	return frame;
}

TEST(HdTest, FreeSilhouetteDrawsOutsideTheFootprintAndRevealsWhatWasBehind)
{
	SDL_Surface *pal = nullptr;
	SDL_Surface *frame = DrawFreeScenario("free", true, 40, &pal);
	ASSERT_NE(frame, nullptr);
	// The art covers the transparent left column of the first sprite (lo pixel (8, 4) is HD (16, 8))
	ExpectColor(At(frame, 16, 8), FrontArt.r, FrontArt.g, FrontArt.b, "the art outside the footprint");
	ExpectColor(At(frame, 17, 9), FrontArt.r, FrontArt.g, FrontArt.b, "the art outside the footprint, its last pixel");
	ExpectColor(At(frame, 18, 8), FrontArt.r, FrontArt.g, FrontArt.b, "the art inside the footprint");
	ExpectColor(At(frame, 14, 8), Background.r, Background.g, Background.b, "outside the art's rectangle");
	EXPECT_EQ(static_cast<uint8_t *>(pal->pixels)[4 * pal->pitch + 8], Own) << "the extension takes the nearest pixel's index";
	// The front sprite's hole shows the back sprite's art, not the front sprite's own pixel
	ExpectColor(At(frame, 34, 10), BackArt.r, BackArt.g, BackArt.b, "the hole shows what was behind");
	ExpectColor(At(frame, 35, 11), BackArt.r, BackArt.g, BackArt.b, "the hole shows what was behind, its last pixel");
	ExpectColor(At(frame, 32, 10), FrontArt.r, FrontArt.g, FrontArt.b, "the front art beside the hole");
	SDL_FreeSurface(frame);
	SDL_FreeSurface(pal);
}

TEST(HdTest, WithoutFreeSilhouetteArtStaysInsideTheFootprint)
{
	SDL_Surface *pal = nullptr;
	SDL_Surface *frame = DrawFreeScenario("bound", false, 44, &pal);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 16, 8), Background.r, Background.g, Background.b, "no art outside the footprint");
	ExpectColor(At(frame, 18, 8), FrontArt.r, FrontArt.g, FrontArt.b, "the art inside the footprint");
	EXPECT_EQ(static_cast<uint8_t *>(pal->pixels)[4 * pal->pitch + 8], BackgroundIndex) << "the PalSurface is as the game drew it";
	const SDL_Color own = Palette()[Own];
	ExpectColor(At(frame, 34, 10), own.r, own.g, own.b, "the hole shows the sprite's own pixel");
	SDL_FreeSurface(frame);
	SDL_FreeSurface(pal);
}

TEST(HdTest, FreeSilhouetteGivesWayToLaterDraws)
{
	SDL_Surface *pal = nullptr;
	// Draws that don't know the HD layer overwrite an extension pixel and a pixel of the sprite after it has drawn
	SDL_Surface *frame = DrawFreeScenario("later", true, 48, &pal, [](SDL_Surface *s) {
		static_cast<uint8_t *>(s->pixels)[4 * s->pitch + 8] = Unique;
		static_cast<uint8_t *>(s->pixels)[5 * s->pitch + 9] = Unique;
	});
	ASSERT_NE(frame, nullptr);
	const SDL_Color unique = Palette()[Unique];
	ExpectColor(At(frame, 16, 8), unique.r, unique.g, unique.b, "an extension pixel overwritten later");
	ExpectColor(At(frame, 18, 10), unique.r, unique.g, unique.b, "a sprite pixel overwritten later");
	ExpectColor(At(frame, 18, 8), FrontArt.r, FrontArt.g, FrontArt.b, "its neighbour");
	SDL_FreeSurface(frame);
	SDL_FreeSurface(pal);
}

TEST(HdTest, FreeSilhouetteStaysOverFoliage)
{
	SDL_Surface *pal = nullptr;
	// Floor foliage drawn after the sprite (the tile beside it, DrawCell) goes under the sprite's extension pixels but over its own
	SDL_Surface *frame = DrawFreeScenario("foliage", true, 52, &pal, [](SDL_Surface *s) {
		const Surface out(s);
		const hd::PillarScope pillar(out, { 0, 11 }, 0, 1);
		const hd::FoliageScope foliage;
		const uint8_t art = Unique;
		const int points[2][2] = { { 8, 4 }, { 9, 5 } };
		for (const auto &pt : points) {
			uint8_t *dst = static_cast<uint8_t *>(s->pixels) + pt[1] * s->pitch + pt[0];
			*dst = Unique;
			hd::Mark(dst, &art, 1);
		}
	});
	ASSERT_NE(frame, nullptr);
	const SDL_Color unique = Palette()[Unique];
	ExpectColor(At(frame, 16, 8), FrontArt.r, FrontArt.g, FrontArt.b, "an extension pixel stays over foliage");
	ExpectColor(At(frame, 18, 10), unique.r, unique.g, unique.b, "foliage still covers the sprite's own pixel");
	EXPECT_EQ(static_cast<uint8_t *>(pal->pixels)[4 * pal->pitch + 8], Own) << "the extension's index is put back";
	SDL_FreeSurface(frame);
	SDL_FreeSurface(pal);
}

TEST(HdTest, OffscreenBuffersCarryTheirArtToTheScreen)
{
	// The control panel is drawn once into BottomBuffer and blitted to the screen every frame: its art must survive the copy
	const fs::path pack = fs::temp_directory_path() / "devilutionx_hd_offscreen";
	const fs::path dump = pack / "dump";
	fs::remove_all(pack);
	fs::create_directories(dump);
	const std::array<SDL_Color, 256> colors = Palette();
	{
		fs::create_directories(pack / "palettes");
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\n";
		WriteArt(pack, "sprite/panel/a/0.0.png", FrontArt, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	ASSERT_TRUE(hd::Enabled());

	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 36, 10, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, BackgroundIndex);
	hd::SetTarget(pal);
	SDL_Surface *buffer = SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_FillRect(buffer, nullptr, BackgroundIndex);
	SDL_Surface *plain = SDL_CreateRGBSurfaceWithFormat(0, 4, 4, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_FillRect(plain, nullptr, Own);
	hd::TrackOffscreen(buffer);

	static std::vector<uint8_t> sprite;
	sprite = Sprite();
	hd::RegisterSpriteList("Panel\\A.CL2", 0, ClxSpriteList { sprite.data() });
	const Surface out(pal);
	ClxDraw(Surface(buffer), { 2, 5 }, ClxSpriteList { sprite.data() }[0]); // buffer x 2..5, y 2..5
	out.BlitFrom(Surface(buffer), MakeSdlRect(0, 0, 8, 8), { 10, 1 });      // screen x 12..15, y 3..6
	ClxDraw(out, { 24, 6 }, ClxSpriteList { sprite.data() }[0]);             // screen x 24..27, y 3..6
	out.BlitFrom(Surface(plain), MakeSdlRect(0, 0, 4, 4), { 24, 3 });       // ... then covered by a copy from an untracked buffer
	hd::Present(nullptr, pal);
	SDL_Surface *frame = LoadFrame(dump);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 24, 6), FrontArt.r, FrontArt.g, FrontArt.b, "the buffer's art after the blit");
	ExpectColor(At(frame, 31, 13), FrontArt.r, FrontArt.g, FrontArt.b, "the buffer's art after the blit, its last pixel");
	ExpectColor(At(frame, 22, 6), Background.r, Background.g, Background.b, "beside it");
	const SDL_Color own = Palette()[Own];
	ExpectColor(At(frame, 50, 8), own.r, own.g, own.b, "a copy from an untracked buffer shows its own pixels");
	hd::ForgetOffscreen(buffer);
	SDL_FreeSurface(frame);
	SDL_FreeSurface(plain);
	SDL_FreeSurface(buffer);
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
}

TEST(HdTest, HellfireMenuArtIsKeptApartFromDiablos)
{
	// LoadPcxSpriteList names a menu file that a Hellfire archive supplies "<name>@hf" (Hellfire's ui_art\heros.pcx has the monk as frame 3,
	// where Diablo's has the new-hero portrait): each name draws its own art only, never the other game's
	const fs::path pack = fs::temp_directory_path() / "devilutionx_hd_menus";
	const fs::path dump = pack / "dump";
	fs::remove_all(pack);
	fs::create_directories(dump);
	const std::array<SDL_Color, 256> colors = Palette();
	{
		fs::create_directories(pack / "palettes");
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\n";
		WriteArt(pack, "sprite/ui_art/heros/0.0.png", { 120, 60, 60 }, index);
		WriteArt(pack, "sprite/ui_art/heros@hf/0.0.png", { 60, 120, 60 }, index);
		WriteArt(pack, "sprite/ui_art/title/0.0.png", { 60, 60, 120 }, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	ASSERT_TRUE(hd::Enabled());

	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 24, 8, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, 0);
	hd::SetTarget(pal);
	const Surface out(pal);
	std::vector<uint8_t> diablo = Sprite();
	std::vector<uint8_t> hellfire = Sprite();
	std::vector<uint8_t> title = Sprite();
	hd::RegisterSpriteList("ui_art\\heros.pcx", 0, ClxSpriteList { diablo.data() });
	hd::RegisterSpriteList("ui_art\\heros@hf", 0, ClxSpriteList { hellfire.data() });
	hd::RegisterSpriteList("ui_art\\title@hf", 0, ClxSpriteList { title.data() }); // Hellfire's own title, no art of its own
	ClxDraw(out, { 0, 3 }, ClxSpriteList { diablo.data() }[0]);
	ClxDraw(out, { 8, 3 }, ClxSpriteList { hellfire.data() }[0]);
	ClxDraw(out, { 16, 3 }, ClxSpriteList { title.data() }[0]);
	hd::Present(nullptr, pal);

	SDL_Surface *argb = LoadFrame(dump);
	ASSERT_NE(argb, nullptr);
	ExpectColor(At(argb, 2, 2), 120, 60, 60, "Diablo's file draws Diablo's art");
	ExpectColor(At(argb, 18, 2), 60, 120, 60, "Hellfire's file draws Hellfire's art");
	const SDL_Color own = colors[Own];
	ExpectColor(At(argb, 34, 2), own.r, own.g, own.b, "a Hellfire file without art keeps its pixels, not Diablo's art");
	SDL_FreeSurface(argb);
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
}

TEST(HdTest, TrackingBelongsToTheThreadThatDraws)
{
	// The loading thread draws art into surfaces of its own (the virtual gamepad's potions) while the main thread has a tracked
	// draw open or is presenting; its draws must not be recorded in the main thread's provenance plane
	const fs::path pack = fs::temp_directory_path() / "devilutionx_hd_threads";
	fs::remove_all(pack);
	fs::create_directories(pack / "palettes");
	{
		const std::array<SDL_Color, 256> colors = Palette();
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\n";
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	unsetenv("DEVILUTIONX_HD_DUMP");
	hd::Init();
	ASSERT_TRUE(hd::Enabled());

	const std::array<SDL_Color, 256> colors = Palette();
	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 52, 12, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	hd::SetTarget(pal);
	static std::vector<uint8_t> sprite;
	sprite = Sprite();
	const ClxSprite clx = ClxSpriteList { sprite.data() }[0];
	{
		const hd::SpriteScope scope(Surface(pal), { 8, 7 }, clx);
		EXPECT_NE(hd::TrackPlane, nullptr) << "the main thread's draw is tracked";

		bool otherThreadUntracked = false;
		bool otherSurfaceDrawn = false;
		std::thread loader([&]() {
			otherThreadUntracked = hd::TrackPlane == nullptr;
			SDL_Surface *own = SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 8, SDL_PIXELFORMAT_INDEX8);
			SDL_FillRect(own, nullptr, 0);
			{
				const hd::SpriteScope inner(Surface(own), { 1, 5 }, clx);
				ClxDraw(Surface(own), { 1, 5 }, clx);
			}
			otherSurfaceDrawn = static_cast<uint8_t *>(own->pixels)[2 * own->pitch + 2] == Own;
			otherThreadUntracked = otherThreadUntracked && hd::TrackPlane == nullptr;
			SDL_FreeSurface(own);
		});
		loader.join();
		EXPECT_TRUE(otherThreadUntracked) << "another thread starts and ends untracked";
		EXPECT_TRUE(otherSurfaceDrawn) << "and draws into its own surface";
		EXPECT_NE(hd::TrackPlane, nullptr) << "the main thread's draw is still tracked afterwards";
	}
	EXPECT_EQ(hd::TrackPlane, nullptr) << "and ends untracked";
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
}

/** Adds a tEXt chunk to a PNG file, right after its header chunk. */
void AddPngText(const fs::path &file, const std::string &keyword, const std::string &text)
{
	std::ifstream in(file, std::ios::binary);
	std::string png((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	in.close();
	const std::string body = "tEXt" + keyword + std::string(1, '\0') + text;
	uint32_t crc = 0xFFFFFFFFU;
	for (const char ch : body) {
		crc ^= static_cast<uint8_t>(ch);
		for (int bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xEDB88320U : 0);
	}
	crc = ~crc;
	const auto size = static_cast<uint32_t>(body.size() - 4);
	std::string chunk;
	for (int shift = 24; shift >= 0; shift -= 8)
		chunk.push_back(static_cast<char>(size >> shift));
	chunk += body;
	for (int shift = 24; shift >= 0; shift -= 8)
		chunk.push_back(static_cast<char>(crc >> shift));
	png.insert(33, chunk); // the 8 bytes of the signature and the 25 of IHDR
	std::ofstream(file, std::ios::binary) << png;
}

/** An effect layer: the two pictures side by side (laid over, added), ``w`` x ``h`` each, every pixel clear but those of ``set`` (x, y, ARGB). */
void WriteEffect(const fs::path &pack, const std::string &file, int w, int h, const std::string &where, const std::vector<std::array<uint32_t, 3>> &set, std::ofstream &index)
{
	fs::create_directories((pack / file).parent_path());
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 2 * w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	SDL_FillRect(s, nullptr, 0);
	for (const auto &px : set)
		static_cast<uint32_t *>(s->pixels)[px[1] * (s->pitch / 4) + px[0]] = px[2];
	IMG_SavePNG(s, (pack / file).string().c_str());
	SDL_FreeSurface(s);
	if (!where.empty()) AddPngText(pack / file, "hdfx", where);
	index << "image " << file << " -\n";
}

constexpr SDL_Color OtherArt = { 60, 120, 60, 255 };

/**
 * A 4x4 sprite at lo pixels x 8..11, y 8..11 with art of one colour and (``withFx``) an effect layer over lo pixels x 6..13, y 4..11, one
 * file pixel per lo pixel: laid over at (6, 4) opaque, (7, 4) half see-through and (9, 9) (on the sprite); added at (8, 4), (10, 10) and
 * (11, 10) (on the sprite), (12, 5). A second sprite with art is there for ``after``, which runs once the first has drawn. The palette has a
 * full-bright entry when ``bright``. Returns the HD frame; ``again`` gets the frame presented after a NewScene.
 */
SDL_Surface *DrawEffectScenario(const std::string &name, bool withFx, bool bright, int width, const std::function<void(SDL_Surface *, ClxSprite)> &after = {}, SDL_Surface **again = nullptr)
{
	const fs::path pack = fs::temp_directory_path() / ("devilutionx_hd_" + name);
	const fs::path dump = pack / "dump";
	fs::remove_all(pack);
	fs::create_directories(dump);
	std::array<SDL_Color, 256> colors = Palette();
	if (bright) colors[255] = { 255, 255, 255, 255 };
	{
		fs::create_directories(pack / "palettes");
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\n";
		WriteArt(pack, "sprite/" + name + "/a/0.0.png", FrontArt, index);
		WriteArt(pack, "sprite/" + name + "/b/0.0.png", OtherArt, index);
		if (withFx) {
			WriteEffect(pack, "fx/" + name + "/a/0.0.png", 8, 8, "-2 -4 8 8",
			    { { 0, 0, 0xFF0A141EU }, { 1, 0, 0x80C86400U }, { 3, 5, 0xFF010203U },
			        { 8 + 2, 0, 0xFF323C46U }, { 8 + 4, 6, 0xFF282828U }, { 8 + 5, 6, 0xFFC8C8C8U }, { 8 + 6, 1, 0xFF323C46U } },
			    index);
		}
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	EXPECT_TRUE(hd::Enabled());

	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, width, 16, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, BackgroundIndex);
	hd::SetTarget(pal);
	hd::NewScene();
	const Surface out(pal);

	static std::vector<uint8_t> a, b;
	a = Sprite();
	b = Sprite();
	hd::RegisterSpriteList(name + "\\A.CL2", 0, ClxSpriteList { a.data() });
	hd::RegisterSpriteList(name + "\\B.CL2", 0, ClxSpriteList { b.data() });
	ClxDraw(out, { 8, 11 }, ClxSpriteList { a.data() }[0]);
	if (after) after(pal, ClxSpriteList { b.data() }[0]);
	hd::Present(nullptr, pal);
	SDL_Surface *frame = LoadFrame(dump);
	if (again != nullptr) {
		fs::remove_all(dump);
		fs::create_directories(dump);
		hd::NewScene();
		hd::Present(nullptr, pal);
		*again = LoadFrame(dump);
	}
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
	return frame;
}

TEST(HdTest, EffectLayerIsLaidOverAndAddedBeyondTheSprite)
{
	SDL_Surface *again = nullptr;
	SDL_Surface *frame = DrawEffectScenario("fx", true, true, 56, {}, &again);
	ASSERT_NE(frame, nullptr);
	ASSERT_NE(again, nullptr);
	// lo pixel (x, y) is the HD block at (2x, 2y)
	ExpectColor(At(frame, 12, 8), 10, 20, 30, "laid over, opaque, above and left of the sprite");
	ExpectColor(At(frame, 13, 9), 10, 20, 30, "its last pixel");
	ExpectColor(At(frame, 14, 8), 150, 100, 49, "laid over by its alpha");
	ExpectColor(At(frame, 16, 8), 150, 160, 170, "added to the background");
	ExpectColor(At(frame, 24, 10), 150, 160, 170, "added right of the sprite");
	ExpectColor(At(frame, 18, 18), 1, 2, 3, "laid over the sprite's art");
	ExpectColor(At(frame, 20, 20), 160, 100, 100, "added to the sprite's art");
	ExpectColor(At(frame, 22, 20), 255, 255, 255, "added up to white");
	ExpectColor(At(frame, 16, 16), FrontArt.r, FrontArt.g, FrontArt.b, "the art where the layer is clear");
	ExpectColor(At(frame, 18, 8), Background.r, Background.g, Background.b, "the background where the layer is clear");
	// A new scene in which the sprite is not drawn has no layer
	ExpectColor(At(again, 12, 8), Background.r, Background.g, Background.b, "gone with the next scene");
	ExpectColor(At(again, 18, 18), FrontArt.r, FrontArt.g, FrontArt.b, "the art is still there");
	SDL_FreeSurface(frame);
	SDL_FreeSurface(again);
}

TEST(HdTest, WithoutAnEffectLayerNothingChanges)
{
	SDL_Surface *frame = DrawEffectScenario("nofx", false, true, 60);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 12, 8), Background.r, Background.g, Background.b, "no layer above the sprite");
	ExpectColor(At(frame, 18, 18), FrontArt.r, FrontArt.g, FrontArt.b, "the art");
	ExpectColor(At(frame, 20, 20), FrontArt.r, FrontArt.g, FrontArt.b, "the art, unlit");
	SDL_FreeSurface(frame);
}

TEST(HdTest, EffectLayerGivesWayToLaterDraws)
{
	// After the sprite: a draw that doesn't know the HD layer changes lo pixel (6, 4), and a sprite with art covers lo pixels x 8..11, y 4..7
	SDL_Surface *frame = DrawEffectScenario("fxlater", true, true, 64, [](SDL_Surface *s, ClxSprite other) {
		static_cast<uint8_t *>(s->pixels)[4 * s->pitch + 6] = Unique;
		ClxDraw(Surface(s), { 8, 7 }, other);
	});
	ASSERT_NE(frame, nullptr);
	const SDL_Color unique = Palette()[Unique];
	ExpectColor(At(frame, 12, 8), unique.r, unique.g, unique.b, "a pixel drawn later without HD art covers the layer");
	ExpectColor(At(frame, 16, 8), OtherArt.r, OtherArt.g, OtherArt.b, "a sprite drawn later covers the layer");
	ExpectColor(At(frame, 14, 8), 150, 100, 49, "the layer beside them");
	ExpectColor(At(frame, 24, 10), 150, 160, 170, "and further off");
	SDL_FreeSurface(frame);
}

TEST(HdTest, EffectLayerFadesWithThePalette)
{
	// No full-bright entry: the palette's brightest channel is 200, so the layer shows at 200 / 248 (206 / 256)
	SDL_Surface *frame = DrawEffectScenario("fxfade", true, false, 68);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 12, 8), 8, 16, 24, "laid over, faded");
	ExpectColor(At(frame, 16, 8), 140, 148, 156, "added, faded");
	SDL_FreeSurface(frame);
}

TEST(HdTest, EffectLayerWithoutItsPlaceIsIgnored)
{
	const fs::path pack = fs::temp_directory_path() / "devilutionx_hd_fxbad";
	const fs::path dump = pack / "dump";
	fs::remove_all(pack);
	fs::create_directories(dump);
	const std::array<SDL_Color, 256> colors = Palette();
	{
		fs::create_directories(pack / "palettes");
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\n";
		WriteArt(pack, "sprite/fxbad/a/0.0.png", FrontArt, index);
		WriteEffect(pack, "fx/fxbad/a/0.0.png", 4, 4, "", { { 0, 0, 0xFF0A141EU }, { 1, 1, 0xFF0A141EU } }, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	ASSERT_TRUE(hd::Enabled());
	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 72, 16, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, BackgroundIndex);
	hd::SetTarget(pal);
	hd::NewScene();
	std::vector<uint8_t> a = Sprite();
	hd::RegisterSpriteList("fxbad\\A.CL2", 0, ClxSpriteList { a.data() });
	ClxDraw(Surface(pal), { 8, 11 }, ClxSpriteList { a.data() }[0]);
	hd::Present(nullptr, pal);
	SDL_Surface *frame = LoadFrame(dump);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 16, 16), FrontArt.r, FrontArt.g, FrontArt.b, "the art, with no layer over it");
	ExpectColor(At(frame, 18, 18), FrontArt.r, FrontArt.g, FrontArt.b, "the art, with no layer over it");
	SDL_FreeSurface(frame);
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
}

TEST(HdTest, EffectLayerOfAPaletteSwapIsItsOwnWhenThePackHasOne)
{
	const fs::path pack = fs::temp_directory_path() / "devilutionx_hd_fxswap";
	const fs::path dump = pack / "dump";
	fs::remove_all(pack);
	fs::create_directories(dump);
	std::array<SDL_Color, 256> colors = Palette();
	colors[255] = { 255, 255, 255, 255 };
	{
		fs::create_directories(pack / "palettes");
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\n";
		// a: a body and its swap, each with an effect layer; b: the same, but the swap has none; c: a body with a layer, drawn without its swap
		for (const char *name : { "a", "b", "c" })
			WriteArt(pack, std::string("sprite/fxswap/") + name + "/0.0.png", FrontArt, index);
		WriteArt(pack, "sprite/fxswap/a@swap/0.0.png", OtherArt, index);
		WriteArt(pack, "sprite/fxswap/b@swap/0.0.png", OtherArt, index);
		const std::vector<std::array<uint32_t, 3>> body = { { 0, 0, 0xFF0A141EU } };
		const std::vector<std::array<uint32_t, 3>> own = { { 0, 0, 0xFF8C1E1EU } };
		WriteEffect(pack, "fx/fxswap/a/0.0.png", 8, 8, "-2 -4 8 8", body, index);
		WriteEffect(pack, "fx/fxswap/a@swap/0.0.png", 8, 8, "-2 -4 8 8", own, index);
		WriteEffect(pack, "fx/fxswap/b/0.0.png", 8, 8, "-2 -4 8 8", body, index);
		WriteEffect(pack, "fx/fxswap/c/0.0.png", 8, 8, "-2 -4 8 8", body, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	ASSERT_TRUE(hd::Enabled());

	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 64, 16, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, BackgroundIndex);
	hd::SetTarget(pal);
	hd::NewScene();
	const Surface out(pal);

	std::array<uint8_t, 256> typeTrn;
	for (int i = 0; i < 256; ++i)
		typeTrn[i] = static_cast<uint8_t>(i);
	typeTrn[Own] = Swapped;
	std::vector<uint8_t> a = Sprite();
	std::vector<uint8_t> b = Sprite();
	std::vector<uint8_t> c = Sprite();
	hd::RegisterSpriteList("FxSwap\\A.CL2", 0, ClxSpriteList { a.data() });
	hd::RegisterSpriteList("FxSwap\\B.CL2", 0, ClxSpriteList { b.data() });
	hd::RegisterSpriteList("FxSwap\\C.CL2", 0, ClxSpriteList { c.data() });
	{
		const hd::NamedRecolour swap("FxSwap\\Swap"); // as InitMonsterTRN applies a type's TRN
		ClxApplyTrans(ClxSpriteList { a.data() }, typeTrn.data());
		ClxApplyTrans(ClxSpriteList { b.data() }, typeTrn.data());
	}
	ClxDraw(out, { 8, 11 }, ClxSpriteList { a.data() }[0]);
	ClxDraw(out, { 24, 11 }, ClxSpriteList { b.data() }[0]);
	ClxDraw(out, { 40, 11 }, ClxSpriteList { c.data() }[0]);
	hd::Present(nullptr, pal);
	SDL_Surface *frame = LoadFrame(dump);
	ASSERT_NE(frame, nullptr);
	// each layer's first pixel is laid over at lo pixel (x - 2, 4), the HD block at (2x - 4, 8)
	ExpectColor(At(frame, 12, 8), 140, 30, 30, "a swap's own layer replaces the body's");
	ExpectColor(At(frame, 44, 8), 10, 20, 30, "a swap without a layer of its own shows the body's");
	ExpectColor(At(frame, 76, 8), 10, 20, 30, "a body drawn without its swap shows its own layer");
	SDL_FreeSurface(frame);
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
}

/** A CLX list of one 16x4 sprite of which only the right column is drawn. */
std::vector<uint8_t> ThinSprite()
{
	std::vector<uint8_t> data = { 1, 0, 0, 0, 12, 0, 0, 0, 30, 0, 0, 0, 6, 0, 16, 0, 4, 0 };
	for (int row = 0; row < 4; ++row)
		data.insert(data.end(), { 15, 0xFF, Own });
	return data;
}

/** In a free pack: the thin sprite at lo pixels x 8..23, y 4..7 with art over its whole rectangle, and an (empty) effect layer or none. */
SDL_Surface *DrawReachScenario(const std::string &name, bool withFx, int width)
{
	const fs::path pack = fs::temp_directory_path() / ("devilutionx_hd_" + name);
	const fs::path dump = pack / "dump";
	fs::remove_all(pack);
	fs::create_directories(dump);
	const std::array<SDL_Color, 256> colors = Palette();
	{
		fs::create_directories(pack / "palettes");
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\nsilhouette free\n";
		WriteArt(pack, "sprite/" + name + "/a/0.0.png", FrontArt, index);
		if (withFx) WriteEffect(pack, "fx/" + name + "/a/0.0.png", 1, 1, "0 0 1 1", {}, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	EXPECT_TRUE(hd::Enabled());
	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, width, 12, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, BackgroundIndex);
	hd::SetTarget(pal);
	hd::NewScene();
	static std::vector<uint8_t> thin;
	thin = ThinSprite();
	hd::RegisterSpriteList(name + "\\A.CL2", 0, ClxSpriteList { thin.data() });
	ClxDraw(Surface(pal), { 8, 7 }, ClxSpriteList { thin.data() }[0]);
	hd::Present(nullptr, pal);
	SDL_Surface *frame = LoadFrame(dump);
	SDL_FreeSurface(pal);
	fs::remove_all(pack);
	return frame;
}

TEST(HdTest, ArtWithAnEffectLayerIsDrawnWhole)
{
	SDL_Surface *bound = DrawReachScenario("reach", false, 76);
	SDL_Surface *whole = DrawReachScenario("reachfx", true, 80);
	ASSERT_NE(bound, nullptr);
	ASSERT_NE(whole, nullptr);
	// The sprite's own pixels are at lo x 23; lo x 8 is 15 pixels from them
	ExpectColor(At(bound, 44, 8), FrontArt.r, FrontArt.g, FrontArt.b, "art next to the sprite's pixels");
	ExpectColor(At(bound, 16, 8), Background.r, Background.g, Background.b, "art far from the sprite's pixels is left out");
	ExpectColor(At(whole, 44, 8), FrontArt.r, FrontArt.g, FrontArt.b, "art next to the sprite's pixels, with an effect layer");
	ExpectColor(At(whole, 16, 8), FrontArt.r, FrontArt.g, FrontArt.b, "art far from the sprite's pixels shows with an effect layer");
	SDL_FreeSurface(bound);
	SDL_FreeSurface(whole);
}

constexpr SDL_Color TipArt = { 60, 60, 120, 255 };
constexpr SDL_Color CornerArt = { 120, 120, 60, 255 };

/**
 * Art for the 4x4 sprite that covers more than it: 12 x 14 original pixels starting 4 left of and 8 above the sprite's top-left (24 x 28 at
 * scale 2). Clear but for the sprite's own rectangle (``FrontArt``, its top-left pixel ``CornerArt``), a tip above it (the sprite's second
 * column, all 8 rows above), a foot below it (its third column, 2 rows) and one pixel 4 left of its second row, all ``TipArt``.
 */
void WriteBoxedArt(const fs::path &pack, const std::string &file, const std::string &box, std::ofstream &index)
{
	fs::create_directories((pack / file).parent_path());
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 24, 28, 32, SDL_PIXELFORMAT_ARGB8888);
	SDL_FillRect(s, nullptr, 0);
	const auto fill = [s](int x, int y, int w, int h, SDL_Color c) {
		SDL_Rect r = { 2 * x, 2 * y, 2 * w, 2 * h };
		SDL_FillRect(s, &r, SDL_MapRGBA(s->format, c.r, c.g, c.b, 255));
	};
	fill(4, 8, 4, 4, FrontArt);
	fill(4, 8, 1, 1, CornerArt);
	fill(5, 0, 1, 8, TipArt);
	fill(6, 12, 1, 2, TipArt);
	fill(0, 9, 1, 1, TipArt);
	IMG_SavePNG(s, (pack / file).string().c_str());
	SDL_FreeSurface(s);
	if (!box.empty()) AddPngText(pack / file, "hdbox", box);
	index << "image " << file << " palettes/test.pal\n";
}

/**
 * The 4x4 sprite at lo pixels x 8..11, y 8..11 of a surface 16 high, with the boxed art above (its tip at lo x 9, y 0..7; its foot at x 10,
 * y 12..13; its far pixel at (4, 9)), in a pack that is ``free`` or not, drawn into the rows from ``clipTop`` down. ``withFx`` adds the
 * effect layer of DrawEffectScenario; a second sprite with art is there for ``after``, which runs once the first has drawn.
 */
SDL_Surface *DrawBoxScenario(const std::string &name, const std::string &box, bool free, int width, bool withFx = false, int clipTop = 0, const std::function<void(SDL_Surface *, ClxSprite)> &after = {}, SDL_Surface **palOut = nullptr)
{
	const fs::path pack = fs::temp_directory_path() / ("devilutionx_hd_" + name);
	const fs::path dump = pack / "dump";
	fs::remove_all(pack);
	fs::create_directories(dump);
	std::array<SDL_Color, 256> colors = Palette();
	colors[255] = { 255, 255, 255, 255 };
	{
		fs::create_directories(pack / "palettes");
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\n";
		if (free) index << "silhouette free\n";
		WriteBoxedArt(pack, "sprite/" + name + "/a/0.0.png", box, index);
		WriteArt(pack, "sprite/" + name + "/b/0.0.png", OtherArt, index);
		if (withFx) WriteEffect(pack, "fx/" + name + "/a/0.0.png", 8, 8, "-2 -4 8 8", { { 0, 0, 0xFF0A141EU }, { 8 + 6, 1, 0xFF323C46U } }, index);
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	EXPECT_TRUE(hd::Enabled());
	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, width, 16, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, BackgroundIndex);
	hd::SetTarget(pal);
	hd::NewScene();
	static std::vector<uint8_t> a, b;
	a = Sprite();
	b = Sprite();
	hd::RegisterSpriteList(name + "\\A.CL2", 0, ClxSpriteList { a.data() });
	hd::RegisterSpriteList(name + "\\B.CL2", 0, ClxSpriteList { b.data() });
	ClxDraw(Surface(pal).subregionY(clipTop, 16 - clipTop), { 8, 11 - clipTop }, ClxSpriteList { a.data() }[0]);
	if (after) after(pal, ClxSpriteList { b.data() }[0]);
	hd::Present(nullptr, pal);
	SDL_Surface *frame = LoadFrame(dump);
	if (palOut != nullptr)
		*palOut = pal;
	else
		SDL_FreeSurface(pal);
	fs::remove_all(pack);
	return frame;
}

TEST(HdTest, BoxedArtIsDrawnWholeBeyondTheSprite)
{
	SDL_Surface *pal = nullptr;
	SDL_Surface *frame = DrawBoxScenario("box", "-4 -8 12 14", true, 84, false, 0, {}, &pal);
	ASSERT_NE(frame, nullptr);
	// lo pixel (x, y) is the HD block at (2x, 2y)
	ExpectColor(At(frame, 16, 16), CornerArt.r, CornerArt.g, CornerArt.b, "the sprite's top-left pixel shows its own part of the picture");
	ExpectColor(At(frame, 18, 16), FrontArt.r, FrontArt.g, FrontArt.b, "the art inside the sprite");
	ExpectColor(At(frame, 23, 23), FrontArt.r, FrontArt.g, FrontArt.b, "the art inside the sprite, its last pixel");
	ExpectColor(At(frame, 18, 14), TipArt.r, TipArt.g, TipArt.b, "the tip right above the sprite");
	ExpectColor(At(frame, 18, 0), TipArt.r, TipArt.g, TipArt.b, "the tip 8 pixels above the sprite");
	ExpectColor(At(frame, 19, 1), TipArt.r, TipArt.g, TipArt.b, "the tip, its last pixel");
	ExpectColor(At(frame, 20, 26), TipArt.r, TipArt.g, TipArt.b, "the foot below the sprite");
	ExpectColor(At(frame, 8, 18), TipArt.r, TipArt.g, TipArt.b, "the pixel left of the sprite");
	ExpectColor(At(frame, 16, 0), Background.r, Background.g, Background.b, "beside the tip, where the picture is clear");
	ExpectColor(At(frame, 20, 14), Background.r, Background.g, Background.b, "above the sprite, where the picture is clear");
	ExpectColor(At(frame, 6, 18), Background.r, Background.g, Background.b, "outside the box");
	ExpectColor(At(frame, 32, 18), Background.r, Background.g, Background.b, "outside the box, right");
	EXPECT_EQ(static_cast<uint8_t *>(pal->pixels)[0 * pal->pitch + 9], Own) << "the extension takes the nearest pixel's index";
	EXPECT_EQ(static_cast<uint8_t *>(pal->pixels)[0 * pal->pitch + 8], BackgroundIndex) << "the PalSurface beside the tip is as the game drew it";
	SDL_FreeSurface(frame);
	SDL_FreeSurface(pal);
}

TEST(HdTest, BoxedArtWithoutFreeSilhouetteStaysInsideTheFootprint)
{
	SDL_Surface *frame = DrawBoxScenario("boxbound", "-4 -8 12 14", false, 88);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 16, 16), CornerArt.r, CornerArt.g, CornerArt.b, "the picture sits where its box says");
	ExpectColor(At(frame, 23, 23), FrontArt.r, FrontArt.g, FrontArt.b, "the art inside the sprite");
	ExpectColor(At(frame, 18, 14), Background.r, Background.g, Background.b, "no tip outside the footprint");
	ExpectColor(At(frame, 20, 26), Background.r, Background.g, Background.b, "no foot outside the footprint");
	SDL_FreeSurface(frame);
}

TEST(HdTest, BoxedArtGivesWayToLaterDrawsAndToTheSurfacesEdge)
{
	// After the sprite: a draw that doesn't know the HD layer changes lo pixel (9, 6), and a sprite with art covers lo pixels x 8..11, y 0..3
	SDL_Surface *frame = DrawBoxScenario("boxlater", "-4 -8 12 14", true, 92, false, 0, [](SDL_Surface *s, ClxSprite other) {
		static_cast<uint8_t *>(s->pixels)[6 * s->pitch + 9] = Unique;
		ClxDraw(Surface(s), { 8, 3 }, other);
	});
	ASSERT_NE(frame, nullptr);
	const SDL_Color unique = Palette()[Unique];
	ExpectColor(At(frame, 18, 12), unique.r, unique.g, unique.b, "a pixel drawn later without HD art covers the tip");
	ExpectColor(At(frame, 18, 2), OtherArt.r, OtherArt.g, OtherArt.b, "a sprite drawn later covers the tip");
	ExpectColor(At(frame, 18, 10), TipArt.r, TipArt.g, TipArt.b, "the tip between them");
	SDL_FreeSurface(frame);
	// Drawn into the rows from 4 down: the tip stops at the surface's edge
	SDL_Surface *clipped = DrawBoxScenario("boxclip", "-4 -8 12 14", true, 96, false, 4);
	ASSERT_NE(clipped, nullptr);
	ExpectColor(At(clipped, 18, 6), Background.r, Background.g, Background.b, "no tip above the surface the sprite is drawn into");
	ExpectColor(At(clipped, 18, 8), TipArt.r, TipArt.g, TipArt.b, "the tip from its first row");
	ExpectColor(At(clipped, 16, 16), CornerArt.r, CornerArt.g, CornerArt.b, "the sprite");
	SDL_FreeSurface(clipped);
}

TEST(HdTest, EffectLayerOfBoxedArtStaysRelativeToTheSprite)
{
	SDL_Surface *frame = DrawBoxScenario("boxfx", "-4 -8 12 14", true, 100, true);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 12, 8), 10, 20, 30, "laid over at the layer's top-left, 2 left of and 4 above the sprite");
	ExpectColor(At(frame, 24, 10), 150, 160, 170, "added right of the sprite");
	ExpectColor(At(frame, 18, 0), TipArt.r, TipArt.g, TipArt.b, "the tip");
	ExpectColor(At(frame, 16, 16), CornerArt.r, CornerArt.g, CornerArt.b, "the sprite");
	SDL_FreeSurface(frame);
}

TEST(HdTest, ABoxThatCantBeReadIsIgnored)
{
	// The picture is then taken to cover the sprite, as any art without a box: nothing is drawn beyond the sprite's rectangle
	SDL_Surface *frame = DrawBoxScenario("boxbad", "-4 -8 0 14", true, 104);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 18, 14), Background.r, Background.g, Background.b, "nothing above the sprite");
	ExpectColor(At(frame, 18, 0), Background.r, Background.g, Background.b, "nothing far above the sprite");
	ExpectColor(At(frame, 8, 18), Background.r, Background.g, Background.b, "nothing left of the sprite");
	SDL_FreeSurface(frame);
}

// Shading (hd.hpp, "Shading"): two palette ramps that a light table darkens by different shares, and a recolour
constexpr uint8_t Other = 40;
constexpr uint8_t OwnLit = 50;
constexpr uint8_t OtherLit = 60;
constexpr uint8_t OwnDim = 70;
constexpr uint8_t OtherDim = 80;
constexpr uint8_t Black = 0;

std::array<SDL_Color, 256> ShadingPalette()
{
	std::array<SDL_Color, 256> colors = Palette();
	colors[Black] = { 0, 0, 0, 255 };
	colors[Other] = { 100, 150, 100, 255 };
	colors[OwnLit] = { 100, 25, 25, 255 };    // Own at half light
	colors[OtherLit] = { 25, 37, 25, 255 };   // Other at a quarter
	colors[OwnDim] = { 160, 10, 10, 255 };    // Own and Other 40 darker, as a light table steps a colour down its ramp
	colors[OtherDim] = { 60, 110, 60, 255 };
	return colors;
}

/** A CLX list of one 8x4 sprite: the left half of every row ``Own``, the right half ``Other``. */
std::vector<uint8_t> TwoToneSprite()
{
	std::vector<uint8_t> data = { 1, 0, 0, 0, 12, 0, 0, 0, 54, 0, 0, 0, 6, 0, 8, 0, 4, 0 };
	for (int row = 0; row < 4; ++row)
		data.insert(data.end(), { 0xF8, Own, Own, Own, Own, Other, Other, Other, Other });
	return data;
}

/**
 * Draws the two-tone sprite (lo pixels x 4..11, y 1..4 of a surface 6 high) through ``trn`` (none: ClxDraw), with flat art ``FrontArt``,
 * in a pack with ``extra`` lines; ``lightTable``: ``trn`` is one of the game's light tables. ``between`` runs after the draw, before the
 * frame is presented. Returns the HD frame (scale 2).
 */
SDL_Surface *DrawShadingScenario(const std::string &name, const std::string &extra, const uint8_t *trn, bool lightTable,
    const std::function<void(SDL_Surface *, const Surface &)> &between = {})
{
	const fs::path pack = fs::temp_directory_path() / ("devilutionx_hd_" + name);
	const fs::path dump = pack / "dump";
	fs::remove_all(pack);
	fs::create_directories(dump);
	const std::array<SDL_Color, 256> colors = ShadingPalette();
	{
		fs::create_directories(pack / "palettes");
		std::ofstream pal(pack / "palettes/test.pal", std::ios::binary);
		for (const SDL_Color &c : colors)
			pal << c.r << c.g << c.b;
		std::ofstream index(pack / "hd.txt");
		index << "scale 2\n" << extra;
		// A monster's art: the game world's (the panels' recoloured art keeps the per-pixel grade)
		fs::create_directories(pack / "sprite/monsters" / name / "two");
		SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 16, 8, 32, SDL_PIXELFORMAT_ARGB8888);
		SDL_FillRect(s, nullptr, SDL_MapRGBA(s->format, FrontArt.r, FrontArt.g, FrontArt.b, 255));
		IMG_SavePNG(s, (pack / "sprite/monsters" / name / "two/0.0.png").string().c_str());
		SDL_FreeSurface(s);
		index << "image sprite/monsters/" << name << "/two/0.0.png palettes/test.pal\n";
	}
	setenv("DEVILUTIONX_HD_PACK", pack.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP", dump.string().c_str(), 1);
	setenv("DEVILUTIONX_HD_DUMP_EVERY", "1", 1);
	hd::Init();
	EXPECT_TRUE(hd::Enabled());
	hd::SetLightTables(lightTable ? trn : nullptr, lightTable ? 256 : 0);

	// Not the size of any other test's surface (nor of this scenario's other calls), so that the provenance plane starts clean
	static int calls = 0;
	SDL_Surface *pal = SDL_CreateRGBSurfaceWithFormat(0, 16 + 4 * calls++, 6, 8, SDL_PIXELFORMAT_INDEX8);
	SDL_SetPaletteColors(pal->format->palette, colors.data(), 0, 256);
	SDL_FillRect(pal, nullptr, BackgroundIndex);
	hd::SetTarget(pal);
	const Surface out(pal);
	static std::vector<uint8_t> two;
	two = TwoToneSprite();
	hd::RegisterSpriteList("Monsters\\" + name + "\\Two.CL2", 0, ClxSpriteList { two.data() });
	if (trn != nullptr)
		ClxDrawTRN(out, { 4, 4 }, ClxSpriteList { two.data() }[0], trn);
	else
		ClxDraw(out, { 4, 4 }, ClxSpriteList { two.data() }[0]);
	if (between) between(pal, out);
	hd::Present(nullptr, pal);
	SDL_Surface *frame = LoadFrame(dump);
	hd::SetLightTables(nullptr, 0);
	fs::remove_all(pack);
	SDL_FreeSurface(pal);
	return frame;
}

std::array<uint8_t, 256> Identity()
{
	std::array<uint8_t, 256> t;
	for (int i = 0; i < 256; ++i)
		t[i] = static_cast<uint8_t>(i);
	return t;
}

/** A channel of ``art`` through the line from (``refA``, ``liveA``) to (``refB``, ``liveB``) (the sprite's two colours), as the layer rounds it. */
int LineGraded(int art, int liveA, int liveB, int refA, int refB)
{
	const double scale = static_cast<double>(liveB - liveA) / (refB - refA);
	const double shift = liveA - scale * refA;
	const int64_t v = ((static_cast<int64_t>(art) * std::lround(scale * 65536)) >> 16) + std::lround(shift);
	return static_cast<int>(std::clamp<int64_t>(v, 0, 255));
}

TEST(HdTest, ALitSpriteIsShadedAsAWholeNotPixelByPixel)
{
	// The light table darkens Own to half and Other to a quarter: per pixel that would draw the original's two halves into the flat art
	std::array<uint8_t, 256> light = Identity();
	light[Own] = OwnLit;
	light[Other] = OtherLit;
	SDL_Surface *frame = DrawShadingScenario("shadewhole", "", light.data(), true);
	ASSERT_NE(frame, nullptr);
	const SDL_Color left = At(frame, 9, 4), right = At(frame, 22, 4);
	ExpectColor(right, left.r, left.g, left.b, "both halves alike");
	ExpectColor(left, LineGraded(FrontArt.r, 100, 25, 200, 100), LineGraded(FrontArt.g, 25, 37, 50, 150), LineGraded(FrontArt.b, 25, 25, 50, 100),
	    "the line through the sprite's (reference, live) colours");
	SDL_FreeSurface(frame);
}

TEST(HdTest, DimLightTakesTheSameBrightnessOffEveryColour)
{
	// A light table that steps both colours 40 down: the art loses 40 too (a ratio would take less off its dark channels, 0.73 x 60 = 44)
	std::array<uint8_t, 256> dim = Identity();
	dim[Own] = OwnDim;
	dim[Other] = OtherDim;
	SDL_Surface *frame = DrawShadingScenario("shadedim", "", dim.data(), true);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 9, 4), FrontArt.r - 40, FrontArt.g - 40, FrontArt.b - 40, "40 darker");
	ExpectColor(At(frame, 22, 4), FrontArt.r - 40, FrontArt.g - 40, FrontArt.b - 40, "40 darker, both halves alike");
	SDL_FreeSurface(frame);
}

TEST(HdTest, ASpriteLitToBlackIsBlack)
{
	// A factor of 0 used to mean "add the offset": art lit to black kept most of its colour (monsters showing in the dark)
	std::array<uint8_t, 256> dark {};
	SDL_Surface *frame = DrawShadingScenario("shadeblack", "", dark.data(), true);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 9, 4), 0, 0, 0, "lit to black");
	ExpectColor(At(frame, 22, 4), 0, 0, 0, "lit to black");
	SDL_FreeSurface(frame);
}

TEST(HdTest, ARecolourKeepsItsRegions)
{
	// Through a TRN that isn't a light table only Own changes: the left edge takes the recolour, the right edge keeps its colour
	std::array<uint8_t, 256> recolour = Identity();
	recolour[Own] = Swapped;
	SDL_Surface *frame = DrawShadingScenario("shaderecolour", "", recolour.data(), false);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 8, 4), 30, 60, 240, "left edge: Own, recoloured to Swapped");
	ExpectColor(At(frame, 23, 4), FrontArt.r, FrontArt.g, FrontArt.b, "right edge: Other, as it is");
	SDL_FreeSurface(frame);
}

TEST(HdTest, PixelShadingKeepsTheOldGrade)
{
	std::array<uint8_t, 256> light = Identity();
	light[Own] = OwnLit;
	light[Other] = OtherLit;
	SDL_Surface *frame = DrawShadingScenario("shadepixel", "shading pixel\n", light.data(), true);
	ASSERT_NE(frame, nullptr);
	ExpectColor(At(frame, 9, 4), FrontArt.r / 2, FrontArt.g / 2, FrontArt.b / 2, "Own: half");
	ExpectColor(At(frame, 22, 4), FrontArt.r / 4, FrontArt.g * 37 / 150, FrontArt.b / 4, "Other: a quarter");
	SDL_FreeSurface(frame);
}

TEST(HdTest, TheViewForgetsArtDrawnInEarlierFrames)
{
	// The game fills what it doesn't draw (unlit tiles) without the layer seeing it; a pixel that keeps its index must not keep its art
	SDL_Surface *frame = DrawShadingScenario("shadeforget", "", nullptr, false, [](SDL_Surface *, const Surface &out) { hd::ForgetView(out); });
	ASSERT_NE(frame, nullptr);
	const SDL_Color own = ShadingPalette()[Own];
	ExpectColor(At(frame, 9, 4), own.r, own.g, own.b, "the pixel's own colour, no art");
	SDL_FreeSurface(frame);
}

} // namespace
} // namespace devilution
